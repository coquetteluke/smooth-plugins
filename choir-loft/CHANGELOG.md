# Changelog

## 1.5.4, 2026-11-12

Fifth pass, Human only. Luke left this call to the lead on 2026-11-11: what Human does above 0 sounds a little different (the voices no longer drift behind you over a long note, voice 2 lets go of a note's end a little later than voice 1, a voice's own wander comes back sooner after a note starts but never takes it more than 40 cents off its note, and it follows a little more of your own movement). Human at 0, the default, is untouched: the working set at Human 0 renders bit for bit as 1.5.3 (starts and breaths at the default and with Humanize and Drift 0, held notes, behind Duo), so do the first session's takes, and the 22 null tests read as before. Saved sessions open as they were and keep their settings. Latency unchanged (659 samples, 13.7 ms at Alto/Tenor). The README section "Human: the held notes with any random numbers (1.5.4)" has the tables.

- The held notes at Human 100 were no worse than at Human 0 only with the plug-in's own random numbers. Run with other random numbers (`REALVOX_SEED` 0 to 7, the take untouched), 1.5.3 failed one of the held note rules (U9) on 5 of 8 runs. 1.5.4 fails on 2 of 8 (two voice notes from 98 to 130 Hz not sung right beyond Human 0's, one allowed, on one run; that and 7 octave verdicts against 4, 6 allowed, on another), and time on the note is above Human 0 on all 8 runs, from 98 to 130 Hz (95.74% to 95.93% against 95.63% to 95.82%) and from 130 Hz up (94.55% to 94.61% against 94.43% to 94.47%).
- Why. Human reshuffled Humanize's random choices: a voice's attack and consonant timing were drawn from Humanize's random numbers where the voice itself started and stopped, and Human's own note end moved those moments, so from then on every note's scoop, detune, glide and place behind the lead was another one than at Human 0 with the same take. They are now drawn where the lead's note starts and ends for that voice, which at Human 0 is the same moment, so nothing changes there and Human 100 can be compared with Human 0 note by note. Human also moved each voice's place behind the lead (a shift per note, and up to 20 ms of falling behind over a long note, given back over the next): a voice hears the lead through its lag, so a lag that drops just after a note change makes it late by the drop. That is out. And Human's own wander could push a voice that was landing or scooping past the 50 cent line; it now never takes a voice more than 40 cents from its note, counting the glide still to go and Humanize's scoop, detune and vibrato.
- The first session's takes at Human 100: 7 late notes against 3 at Human 0 (5 allowed) to 3. Voice 2 landed 85 to 90 ms into four held notes because its lag dropped just after it heard the note change.
- So a voice still moves as much inside a held note: the wander comes back over 80 ms after a note starts (was 150; the 40 cent reach now does what that was for), and each voice follows half as much again of your own movement. The ends of the two voices stay apart now that voice 2 no longer falls behind: voice 2 holds the end of a note up to 34 ms (was 24) and dies away over 14 ms (was 18); voice 1 lets go at once (was up to 5 ms) and dies away over 4 ms (was 10). No voice holds on longer than in 1.5.3 (suite: 28 ms longer than at Human 0, was 40; 50 allowed).
- Human 100, working set, the plug-in's own random numbers: pitch spread inside a note 10.92 cents (10 or more), movement in common 0.28 (0.30 or less; 0.24 to 0.28 over four runs), level curve 0.52 (0.65 or less), ends apart 13 ms (8 or more), U1 to U8 and U9 pass. Human 35 and 65: U1 to U8 and U9 pass. The first session's takes: pitch spread 10.71, movement in common 0.23, level curve 0.62, ends apart 9 ms, U1 to U8 and U9 pass.

Not at zero, and why (the report has the numbers). The held note rule's one-note slack on 98 to 130 Hz (H3) and its two-verdict slack on octave errors (H7) are smaller than what five voice notes of the working set do by themselves: on those, the tool reads the voice 50 cents or more off for 30 ms and then on its note an octave away, and they come and go with the random numbers at Human 0 too. Human 0 run against Human 0 with another of the eight seeds fails U9 in 16 of 56 pairs (H3, H5 or H7). Real breathy starts at the 90th percentile (46 ms with Humanize and Drift 0, target 44), fry phrase starts (50 ms, target 44) and the first session's leak (2.47% against 1.5.0's 2.28%) are changes at Human 0 and were not made. The fry starts were traced to the restart on a jump up out of a creak (1.5.1): the same rule without restarting a start's settling reads 44 ms on fry phrase starts and passes the decidable rule, but moves 433 of 2412 starts at the default, two of them 46 and 76 ms later, so it is measured and left out.

Tests: 663 pass, 0 fail, as 1.5.3. On the suite's own held notes at Human 100 the longest hold over Human 0 is 28 ms (was 40, 50 allowed), the two voices' ends 32.0 ms apart (32.5), the interval between the voices 14.83 cents (14.89, 15 allowed). No limit was changed. No parameter, AU code, Input Type value or preset changed.

## 1.5.3, 2026-11-12

Fourth pass, Human only. Luke left this call to the lead on 2026-11-11: what Human does above 0 sounds a little different (each voice follows its own part of your movement, and voice 2 keeps more of its own level). Human at 0, the default, is untouched: every render of the working set at Human 0 is bit for bit 1.5.2's (starts and breaths at the default and with Humanize and Drift 0, held notes, behind Duo), and so are the 22 null tests. Saved sessions open as they were and keep their settings. Latency unchanged (659 samples, 13.7 ms at Alto/Tenor). The README section "Human: what the two voices share (1.5.3)" has the tables.

- The two voices' movement in common at Human 100 sat on its limit: 0.30 on the working set (0.30 allowed) and 0.31 on the Rode blind set. The same version run with other random numbers (the take untouched) read 0.30, 0.30, 0.34 and 0.27 on the working set, so the blind set's miss was chance landing on the wrong side of a number with no room. What the two voices shared was the lead: both copied the same movement of it, through Follow Lead and through Human's own follow (with no following at all it read 0.26; a lead that does not move gives 0.04). Now each voice follows its own part of that movement: voice 1 (and every odd numbered voice) the slow part, voice 2 (and every even numbered one) the quick part, for Human's own follow and, by Human's amount, for what Follow Lead copies within 18 cents of the note. A slide from one note to the next is still copied whole by both, and the bend with the lead out of a start is untouched: split there too, voice 1 lagged the lead through slides and sang off its note for 600 more 5 ms frames of the working set. Human's own follow is twice as strong, since it is no longer shared. Working set, Human 100, four runs: 0.27, 0.27, 0.29, 0.24 (1.5.2: 0.30, 0.30, 0.34, 0.27). First session's takes: 0.18 (0.21).
- Voice 2 now keeps 0.6 of its own level through your quick swells and dips at Human 100 (was 0.3; voice 1 keeps 0.7 as before). Level curve like the lead's: working set 0.59 to 0.53 (0.51 to 0.53 over the four runs), first session's takes 0.68 to 0.61, goal 0.65 or less. Level against the lead inside a note, spread 0.95 to 1.02 dB (limit 1.21).
- At Human 100 on the working set every goal and limit holds: pitch spread 10.47 cents (10 or more), in common 0.27 (0.30 or less), level curve 0.53 (0.65 or less), ends apart 12 ms (8 or more), U1 to U8, and the held notes no worse than at Human 0 (U9: 98 to 130 Hz 698 of 700, time on note 95.63% and 94.41% against 95.68% and 94.47%, 6 octave verdicts against 5, 1 late against 1). Human 35 and 65 hold every limit too, U9 included (1.5.2 missed U9 at 35 by one octave verdict).

Not changed, and why (measured, the report has the numbers): real breathy and fry starts (90th percentile 46 and 50 ms with Humanize and Drift 0, 46 to 48 and 44 to 54 over four runs; 1.5.0 reads 44 to 48 and 44 to 60 the same way); the 1.5.1 restart on a jump out of a creak delays three of the starts that cross 44 ms and saves others by up to 136 ms, and how long the creak lasted does not tell them apart. The first session's leak (2 frames of a 950 Hz squeal in take 04 at 70.79 s, held 10 ms longer by 1.5.1's start rule). The first session's late notes at Human 100 (7 against 3 at Human 0): voice 2 standing further back lands 85 to 90 ms into three notes; giving back its trail faster at a new note was built and measured and did not fix it (6, and a time on note rule failed), so it is not in. The held note rules at Human 100 move with chance: with other random numbers 1.5.2 fails one of them in one run of four and 1.5.3 in two of four (8 octave verdicts against 5; time on the note from 98 to 130 Hz 95.64% against 95.66% allowed).

Tests: 663 pass, 0 fail, as 1.5.2. No limit was changed. No parameter, AU code, Input Type value or preset changed.

## 1.5.2, 2026-11-11

Third pass. What 1.5.1 made worse on Luke's takes is back where 1.5.0 had it or better, Human at 100 reaches all four of its goals, and there is an output ceiling. Latency unchanged (659 samples, 13.7 ms at Alto/Tenor). The README section "The leak, the note ends and the ceiling" has the tables.

- The leak inside phrases, found frame by frame. 1.5.1 let the period of a start be twice the first deep dip, adopted it at once and let its deeper reading count for the votes. On take 02 at 189.41 s that called 5 ms of a murmur at -46 dBFS a note (516 Hz reading 0.37 to 0.48, twice its period 0.18 to 0.22) and the voices sang 30 ms of it. The whole rise was that kind of event: 5 frames in one gap on the working set, 1 frame on the Rode blind set, 5 frames in 4 gaps on the first session. Now the longer period must come out the same twice in a row while nothing is voiced, and the analysis held back counts as a vote if it would have been one, so no start is called later. Working set, Humanize and Drift 0: 4.27% back to 3.76% (1.5.0: 3.76%). The tracker alone is voiced for 156 ms in the gaps between words, as 1.5.0 (1.5.1: 161).
- Starts out of a creak. 1.5.1 refused any low start when the range had a note at half its period; that was written for a word's last creak running into the next word behind a doubler, but it also refused a fresh start out of a creak (take 10 at 23.69 s, take 05 at 115.25 s), where the votes then flipped between the creak and the note for 40 ms with no voice at all. The refusal now applies only within 100 ms of a followed low note's end. Time on the note under 98 Hz: 86.6% to 87.8% (1.5.0: 87.5%), 28 of 28 held notes. Behind Duo (takes 02, 10, 12): 4 octave errors (1.5.1: 5, 1.5.0: 14, 1.4.4: 8), 889 of 896 voice notes above 130 Hz sung right.
- Kept: the three breathy high start tests read 38.7, 36.0 and 33.3 ms as in 1.5.1 (limit 40), after a creak 8 ms (limit 20), held notes above 130 Hz 2623 of 2630.
- Ceiling, a new switch, off by default, a real AU parameter (`ceiling`, added right after Human, so every older one keeps its place; presets leave it alone; older sessions open with it off). The voices add level (a lead peaking at -1 dBFS comes out at +3.9 dBFS at the default and +5.5 on Full Stack) and nothing held the output under full scale. On, no sample passes -0.3 dBFS: one gain for both channels comes down at once when a sample would pass a soft knee, holds 30 ms and comes back over 200 ms. No look ahead, no added delay. Measured with the lead at -1 dBFS: highest sample -0.304 dBFS (default) and -0.300 (Full Stack), true peak -0.29 and -0.26 dBTP; inside a held note it adds -52 and -44 dB beside a plain turn down; a lead at -19 dBFS comes out bit for bit the same with it on; Bypass with it on passes a vocal at -0.1 dBFS untouched.
- The window: the Ceiling switch sits in the top row beside the voice count, and to its left the output's loudest moment in words ("Out -3.0 dB"). From -1 dBFS up the words turn warm white and say "near full scale", above 0 "over full scale", whether the ceiling is on or not. Nothing overlaps and nothing is cut off at 1 to 8 voices with Advanced open or closed.
- Human, each voice's own note end. A voice used to end where the lead's note was called over, so the two voices ended 4 to 6 ms apart whatever else Human did. Now, when the lead's note is over for a voice, the voice holds on for its own time on the lead's last periods (its read point stands still, so it never reads into the consonant or the gap) and dies away over its own time, both drawn per voice and per note from that voice's own random numbers and scaled by Human: voice 1 up to 5 ms and 10 ms, voice 2 up to 24 and 18 ms at 100. The hold is over at once when the lead is pitched again or about to be, or has not fallen 2 dB after 10 ms, so nothing trails into the next note.
- Human at 100 on the working set: ends apart 6 to 12 ms (8 wanted), pitch spread inside a note 10.35 cents (10 wanted), the two voices' movement in common 0.30 (0.30 or less), level curve like the lead's 0.59 (0.65 or less). U1 to U8 hold at 35, 65 and 100. U9, the held notes no worse than at Human 0, holds at 65 and 100 (at 100: 98 to 130 Hz 699 of 700 and 5 octave verdicts, both as at 0) and misses at 35 by one octave verdict (8 against 5, 7 allowed). Those verdicts flicker: three short notes where the tool's reading of a voice's pitch already flips for single frames at Human 0 and ran 30 ms at 35; the run before read 4 at 35. To get there with the note ends in: slow wander 11 to 12.8 cents, the wander 10% at the start of a note and back over 150 ms (was 35% and 100 ms), a fifth less follow of the lead, and a voice at most 7 ms later behind the lead (was 11). Human at 0 is not touched: none of it runs.
- A breathy start of the real shape, a new test beside the old one (`PluginTest realstarts`). Luke's 89 breathy starts on the working set, measured with the new `RealVox onsetshape`: notes at 155 Hz (half between 137 and 193), 30 ms of loud breath with no pitch in it, then the note at once, clean within 20 ms, in from 40 to 70 cents under. The old synthetic start is a note of up to 587 Hz with breath all through its first 80 ms, which is why 1.5.1's fix for it never reached his starts. The new start passes with the same limits: 95th percentile -4.4 ms (default), 12.8 ms (Tight), 4.1 ms (Humanize 0), no wrong first notes.
- The start and breath rule of 1.4.2 is unchanged and still cannot be passed at any setting: one version against itself, with the take slid by 21 ms or with another seed, fails 2 to 9 of its rules. Beside it there is now a rule that can be decided (`realvox/decidable.py`): each of the same 21 measures no worse than the reference by more than that measure's own noise, measured from five slid runs and three reseeded runs (`realvox/NOISE-default.txt`, `NOISE-plain.txt`). By it 1.5.2 is no worse than 1.5.0 or 1.4.4 on any measure of the working set, at the default and with Humanize and Drift 0.

Still above the better of 1.4.4 and 1.5.0 on the working set with Humanize and Drift 0, each inside its measure's noise and none traced to a cause in the plug-in: wrong first notes at phrase starts 8 (6 and 7; noise 4; the same nine starts flicker in every version), fry phrase starts 90th percentile 50 ms (44; noise 16; the two slowest are 32 and 18 ms faster than in 1.5.0), breathy starts 46 ms (44; noise 10), starts after a consonant 26 ms (24; noise 4), breath leak 12.19% (12.12; noise 0.69). Real breathy starts did not get faster: the tracker is on the right note 45 ms after the onset at the 90th percentile, as in 1.5.0 (1.4.4: 57), median 24.

Tests: 663 pass, 0 fail (1.5.1: 616). New: 19 for the ceiling, 13 for its switch and light in the window, 12 for the real shaped start, 3 for the note ends. The 22 null tests still read as 1.5.1 at Human 0 with the ceiling off. No limit was changed. No existing parameter, AU code, Input Type value or preset changed. On Luke's takes the default does not render bit for bit as 1.5.1: the two tracker fixes above change where voices start.

## 1.5.1, 2026-11-11

Zero failing tests. Luke, on 1.5.0: "This is not good enough. I need 100% and I need zero failures." The three breathy high start tests that had failed since 1.4.0 pass, at the same latency; the octave low behind a doubler is fixed; Human at 100 reaches three of its four goals. The README section "Breathy high starts, the creak and the doubler" has the tables.

- Breathy high starts, 95th percentile time to sound past the lag, limit 40 ms: default 67.9 to 38.7 ms, Tight 72.0 to 36.0, default with Humanize 0 67.1 to 33.3. Wrong first notes stay 0 of 432, and breaths still make no voice sing. Latency unchanged: 13.7 ms Alto/Tenor, 9.0 Soprano, 20.6 Low Male and Wide Range. No setting with more latency was needed.
- Cause one, the period. Breath that has gone through the voice's own formants rings at them for a few ms, and on a high note the period is that short, so half a period, one period and three periods read 0.27, 0.27 and 0.12 on a breathy A4. The tracker took half, refused it, waited, then started a twelfth low. Until a note is established its period is now the shortest of the deep dips and their halves, thirds and quarters that a voice could have (`startPeriod` in `src/PitchTracker.h`), and a note just called does not drop to a multiple of its period while its own still holds.
- Cause two, the reading. On a breathy start the period read 4% either way from one period to the next. For the first 80 ms of a note with a period under 5 ms that is still breathy 10 ms in, the period given out is the coarse one averaged over 10 ms and refined at a long lag, and the marks keep to it.
- Cause three, the voice. A grain cut from a lead that is half breath is half breath; the lead itself is not within 50 cents of its note by the test's measure until 46 ms in. During such a start each grain is the mean of three periods (`steady` in `src/VoiceShifter.h`). You can hear this: for the first 80 ms of a breathy start above 200 Hz the voices carry less breath than the lead.
- After a creak. A word that ends in a creak an octave down is followed under the range since 1.5.0. Behind Duo the next word was then sung an octave low for its first 30 to 40 ms: of 896 voice notes above 130 Hz on takes 02, 10 and 12, 13 an octave off in 1.5.0 against 7 in 1.4.4. Now 4, with 890 sung right (1.4.4: 889, 1.5.0: 879). A followed low note gives way as soon as the range has a clean note at half its period, and a new low start is refused when the range's dip at half its period has even harmonics that do not flip. New test `PluginTest aftercreak` with its limit in `tests/PassLimits.h`: 1.5.0's tracker reads the word an octave low for 42 and 47 ms, 1.5.1 for 0 and 8, limit 20.
- On the dry takes the voices glided up an octave out of such a creak: two held notes reached 110 and 120 ms late in 1.5.0. A jump up of a sixth or more out of a note under the range is a new start. Held notes above 130 Hz at the default: 2623 of 2630, as 1.4.4 (1.5.0: 2621), late 0.
- Human at 100, goals from 1.5.0: the two voices' movement in common 0.32 to 0.27 (0.30 or less wanted, reached), level curve like the lead's 0.64 to 0.58, pitch spread 10.3 cents. Ends of the two voices apart 4 to 6 ms: 8 wanted, NOT reached. The voices follow half as much of the lead, each voice's vibrato runs at its own speed, and a voice falls up to 20 ms behind over a long note. Every limit U1 to U9 holds at 35, 65 and 100.

What it costs on Luke's 11 working takes: time on the note under 98 Hz 87.5% to 86.6% (still 28 of 28 held notes). With Humanize and Drift at 0: voices singing through unpitched frames inside phrases 3.76% to 4.27% (1.4.4: 3.56%; the measure moves to 4.09% and 4.23% by itself when 1.4.4's take is slid), fry starts 90th percentile 44 to 50 ms. Wrong first notes at phrase starts 7 to 6, breath leak 12.24% to 12.19%.

Tests: 616 pass, 0 fail (1.5.0: 611, 3). 2 new checks (after a creak). The 22 null tests pin 1.5.1 at Human 0: starts changed on purpose, and a clean start is called a few ms sooner too, so every preset moved, Tight included. No limit was changed. No parameter, AU code or Input Type value changed. The window did not change.

## 1.5.0, 2026-11-11

Low notes get sung, and there is a Human control. Luke, on 1.4.4: "it misses low notes a lot and the voices sound robotic like really robotic. i mean i like the robotic sound but i also want a more human option". Both were measured on 11 of his own dry takes (Rode NT1, whole songs) with `realvox/RealVox.cpp`. Its checks and pass rules were committed before any change was measured (1f8e91e for the held notes, 772d144 for Human). The README section "Low notes and Human" has the tables.

- Low notes. Alto/Tenor, the default Input Type, tracked from 98 Hz, and Luke's low notes sit at 91 to 97 Hz. Of 28 held voice notes under 98 Hz, 1.4.4 sang 6 (12 missed, 8 dropped out part way, 2 on another note) and 1.5.0 sings 28. Time on the note there: 42.0% to 87.5%. All 22 failures had the one cause, the note was under the range. From 98 to 130 Hz: 697 of 700 to 700. Above 130 Hz: 2623 of 2630 to 2621 (two voice notes reached 110 and 120 ms late).
- The latency stays 13.7 ms. The range's own window is untouched, so starts are called as before. Only where it finds nothing periodic, or while a low note is being followed, the tracker also looks at periods from 98 down to 65 Hz over a longer window (`lowDip` in `src/PitchTracker.h`). A voice on such a note reads up to one period later, which is the voice's lag, not the plug-in's delay. Under the range it is choosier, so fry and creak mostly still do not sing. Input Type keeps its four choices and their values.
- Human, a new control, 0 to 100%, 0 by default, a real AU parameter (`human`, added last, so every other parameter keeps its place). At 0 none of it runs. Above 0 each voice gets, from random numbers of its own: a pitch wander and flutter inside each note, a small part of the lead's own movement on the note, a vibrato whose rate and depth drift, a level that keeps part of its own through the lead's swells, and a small per note shift in how far behind the lead it stands. Measured first: against Luke himself and against Luke singing a part twice, the voices' pitch inside a note was 3.8 times stiller (5.3 against 20.1 cents), their level a copy of his (0.81 alike against 0.13), and the two voices moved as a block (0.42 against 0.12). Note changes, throat and consonants were not causes and are left alone.
- Human at 35, 65 and 100 on the 11 takes: pitch spread inside a note 6.2, 8.0 and 10.4 cents (0: 5.3, Luke: 20.2); level curve like the lead's 0.78, 0.74, 0.64 (0: 0.82); note average off its note 5.0, 6.6, 7.8 cents (0: 4.6, limit 10); interval between the voices, spread 7.3, 9.5, 12.2 cents (limit 15); starts after the lead 46 and 53 ms at every setting (limit 60). Limits U1 to U8 hold at all three. U9 fails at 35 on one rule (2 of 700 held notes from 98 to 130 Hz read an octave off for 55 and 30 ms; the one traced was the tool's reading, not the voice). Two of the four "more human" numbers at 100 were not reached: the two voices still move together 0.32 (wanted 0.30 or less) and end 4 ms apart (wanted 8 or more).
- Presets leave Human where it is. Tight puts it at 0. Sessions from before 1.5.0 open with it at 0.
- A click is gone. At a note start, a voice's copy of the consonant before the note jumped to the note's new place behind the lead while still fading out. 2 of the 40 clicks the shared tool listed on Luke's takes were this (take 05 at 175.2 s, take 06 at 111.6 s, steps of 0.004); now 0.0004 and 0.0007. The other 38 are clicks and creaky pulses in the takes, sung back by a voice 4 to 59 ms later.
- The window: Human sits in the bottom row with Humanize and Vibrato, which now share the first two columns; Harmony and Mix keep theirs. No label or value is cut off at any voice count, with Advanced open or closed.

What the low note fix costs, with Humanize and Drift at 0 so nothing is random: voices sing through 12.24% of breath frames (1.4.4: 12.12%) and 3.76% of unpitched frames inside phrases (3.56%); starts after a consonant, 90th percentile 28 ms (24); breathy starts, median 20 ms (18); one more wrong first note at phrase starts (7, was 6). Better: dropouts 2.17% to 1.48%, phrase starts 90th percentile 62 to 56 ms. The 1.4.2 pass rule cannot be met by any change this small: 1.4.4 measured against itself with the take slid by 21 or 42 ms fails 9 or 10 of its rules.

Behind a doubler (Duo first, takes 02, 10 and 12): both versions lose about 3 points of time on the note, and 1.5.0 reads an octave low more often than 1.4.4 there (13 voice notes above 130 Hz against 7, of 896). Choir Loft was not tuned for a doubled lead; it does better before Duo.

Human at 0 against 1.4.4. The 1.5.0 source with the low note fix and the click fix left out renders all 11 takes bit for bit as 1.4.4, at the default, Full Stack and Tight (33 of 33). With them in: at Soprano, Low Male and Wide Range only the click fix differs, 40 to 55 dB under the render. At Alto/Tenor a render is the same file up to the first moment the tracker follows something under 98 Hz (0.5 to 23.5 s into these takes), and a different file from there on: the voices' pitch pulses and their note by note random choices carry on from another place. Same measures, not the same samples.

Tests: 30 new checks (the Human parameter, sessions and presets, in tune and not sloppy at 35 and 100, no clicks, the window). 611 pass, 3 fail (1.4.4: 581, 3). The 3 are the breathy high starts as before: 67.9, 72.0 and 67.1 ms against 40. Of the 22 null tests, the low note fix and Human at 0 changed none; the click fix changed them for every preset but Tight, and those now pin 1.5.0 at Human 0. Two new checks were changed before they were first committed, because they measured something other than the limit they stood for: "a voice moves on a long note" from 10 cents to 1.4 times Human 0, and the interval check from the whole note to 0.6 s windows.

Not fixed: the breathy high starts. Latency unchanged. No existing parameter, AU code or Input Type value changed.

## 1.4.4, 2026-11-10

Your own voice has a card. Luke asked why he could not see his voice in the row of voice cards: only the added singers had one.

- A Lead card comes first in the row, in the lead's warm white: its name with the same switch dot as a voice, the note you are singing, Level and Pan. Where a voice says its interval, it says "You, as sung", because the lead is not changed.
- Lead and Lead Pan moved into that card, so each is still in the window once. The bottom row is now Humanize, Vibrato, Harmony and Mix, on the same four columns as Advanced. In Advanced, "Consonants and lead" is now "Consonants", and Follow Key or Chord Hold moves up into the row Lead Pan left.
- The voice cards are a little narrower to make room: 240 wide up to four voices (was 272), 120 at eight (was 132). The longest interval, "Octave down", still fits: at eight voices its letters are narrowed by 1.7%, and a test now checks every interval at every voice count and Mode.
- No change to the sound, the parameters or saved sessions.

Tests: 10 new window checks (the card shows at every voice count, sits left of voice 1, lines up with a voice's rows, each lead control is in the window once, the switch and the note, every interval's words fit).

## 1.4.3, 2026-11-10

Picking a Key or Scale in the window now turns Follow Key off. Luke had Follow Key on, picked E Major, and the window kept showing A Minor and the voices kept singing in it: with Follow Key on, the key comes from Key's last reading and a pick by hand changed nothing you could see or hear.

- A pick from the Key or Scale list, or a double-click reset of either, switches Follow Key off as one gesture, so the last thing chosen is what the voices use. Automation of Key or Scale does not touch Follow Key, and picking a Chord or Type does not either.
- No change to the sound, the parameters or saved sessions.

Tests: 8 new window checks for this.

## 1.4.2, 2026-11-09

Breaths no longer make the voices sing, and voices come in sooner at the start of a word. Measured on Luke's own dry takes as well as the tests (`realvox/`, see its README).

- A start must also repeat once its formants are taken out (`residualDip` in `src/PitchTracker.h`): linear prediction over the last 24 ms, then whether what is left repeats a period later. A voice leaves a train of pulses; breath through a vowel is noise again. Plainly periodic sound (dip under 0.05) and a run that has stayed clean for 10 ms skip the check.
- A start is written back to where nearly periodic sound began, or to where the plain tests would have called it, whichever is earlier, but never over the last stretch's pitch. A voice standing behind the lead reads that far back, so it comes in on time.
- On a breathy high note (reading over 600 Hz) the true period wins over half of it when it is 0.015 deeper over three windows. Fixes the octave high first note on breathy A4 starts.
- While a start settles, a voice changes note 3 cents past the half way point instead of 12, so a vibrato that starts on the line between two notes does not hold the wrong one for 20 ms.

Tests, 1.4.1 to 1.4.2: breaths, s, sh, noise and room tone, worst share of time a voice sings 29.8% to 0, longest 70 ms to 0. Voices singing another note first: default 1 to 0, Tight 1 to 0, Humanize 0 stays 0. Hard starts 95th percentile 17 to 14 ms. `say` starts median 21.7 to 21.4 ms. 563 pass, 3 fail (559, 7).

Luke's takes, 1.4.2 against 1.4.1, measured by `realvox/RealVox.cpp` (default preset, key and tuning found from each take):
- Dev takes (01 02 03 04 06 07 09): breath leak 9.7% to 2.6% of breath frames, wrong first notes at phrase starts 29 to 24 of 424, phrase starts median 26.4 to 24.5 ms, word starts median 24.0 to 21.8 ms, leak inside phrases 3.1% to 1.0%. Worse: phrase starts never reaching level 8 to 11 of 424, fry phrase starts 90th percentile 41.7 to 48.2 ms.
- Held out takes (05 08 10 11), run once each after the tool was committed: breath leak 12.0% to 3.1%, phrase starts median 22.7 to 22.0 ms and 90th percentile 46.4 to 41.8 ms, word starts median 22.5 to 19.9 ms, leak inside phrases 8.6% to 2.7%. Worse: wrong first notes at phrase starts 5 to 6 of 190 and at word starts 28 to 32 of 476; breathy starts 90th percentile 52.0 to 52.6 ms.

Still failing: breathy starts on high notes, 95th percentile 67.9 ms (default), 72.0 (Tight) and 67.1 (Humanize 0) against 40 (1.4.1: 73.5, 72.0, 73.3). The aspiration sits at the voice's own level for the first 50 ms and the tracker cannot tell the note from it sooner.

The 22 null tests now pin 1.4.2's sound, since starts changed on purpose, as in 1.4.0. Latency unchanged, 13.7 ms at the default. No parameter changed.

## 1.4.1, 2026-11-08

Breaths sing less. A voice now starts only when the lead's even harmonics repeat as well: the sound plus itself half a period later cancels a lone formant's ring, and a voice's 2nd, 4th... harmonics still repeat a period later (`evenDip` in `src/PitchTracker.h`, fresh starts only).

- Breaths through a closed vowel no longer make a voice sing. Worst breath now, through an open vowel: 29.8% of the time, longest 70 ms (1.4.0: 54.5%, 190 ms, limit 1% and 20 ms, still failing).
- Voices singing another note first at the default preset: 2 to 1 of 432. Tight breathy starts, 95th percentile: 76 to 72 ms.
- Hard starts, 95th percentile: 26 to 17 ms at the default, 28 to 20 ms on Tight. Starts after a 100 ms gap and s into the vowel are about 3 ms later at the median, under their limits. `say` starts: all 58 sound, median 21.7 ms (1.4.0: 21.3).

Tried and dropped: a 30 ms periodicity check passed the breath test fully but made starts after a consonant, fry or breath 10 to 18 ms later and `say` starts 4.7 ms later. Correcting octave high readings by doubling them let more breath through.

Still failing: breathy starts on high notes (72 to 73 ms against 40), breaths through an open vowel, and one wrong first note each at the default (breathy) and Tight (vibrato starting exactly between two notes). Latency unchanged. No parameter changed.

## 1.4.0, 2026-11-08

Voices come in on the right note at the start of a phrase. Luke heard Choir Loft miss vocal starts on his real vocals in Logic.

What caused it, measured with a new test (`tests/OnsetTests.h`, 432 voice starts per preset, 17 kinds: hard, soft, scoops, breathy, creaky, s t m l into the vowel, vibrato at once, short first notes, a short gap, room reverb, quiet and bright takes, every Input Type, 3 pitches each, at the default preset, Tight and Humanize 0):

- The pitch tracker's first few ms of a note can read an octave high, while its window is still half silence. The voices then picked a note an octave up and took about 100 ms to glide down from it. 1.3.1: hard starts reached their note up to 148 ms late.
- Voices looked back into the first, least sure ms of pitch for as long as they stand behind the lead, so a wrong first reading came back after the tracker had fixed it.
- Each new word after a consonant, a breath or a room's tail glided over from the last note instead of starting on its own.
- A scoop into a note sang the scale note under it first, then jumped.
- Breathy sound made the tracker jump an octave or a twelfth down for a few ms, and every jump started a glide back.
- After any start the level match took up to 150 ms to come round.

What changed:

- Pitch for the first 30 ms of a stretch is rewritten with the newest reading, and a voice whose look back lands before the stretch began takes the newest reading.
- Starts inside a phrase (after 10 ms without pitch, or when the lead comes in 9 dB louder) are treated like phrase starts. In a start's first 50 ms plus the voice's lag, and after a note held under 80 ms, notes change without a glide. Glides are for legato notes.
- While the lead rises out of a start the voices bend with it, letting go within about 20 ms once it stops rising.
- The tracker moves to two or three times its period, once a note is under way, only when that also holds over three windows.
- The level match reads 7 times faster for 60 ms after a start.

Results, 1.3.1 to 1.4.0, default preset: voices singing another note first 115 to 2 of 432; time to sound past the voice's lag, worst kind's 95th percentile, 288 ms to 74 ms; hard starts 148 to 26 ms. Tight: 100 to 1 and 168 to 76 ms. Humanize 0: 111 to 0 and 252 to 73 ms. No voice missed a first note before or after.

Still failing: breathy starts on high notes (Soprano and Wide Range, A4 and D5, aspiration at -10 dB through the vowel's formants) still read an octave high for about 50 ms, 95th percentile 73 to 76 ms against 40. Breaths through a closed vowel at -30 dBFS make a voice sing 54.5% of the time against 1% (1.3.1: 55.1%). A new test for that was added; it failed before and fails after.

The 22 null tests that pinned the sound to 1.1.0 and 1.2.0 bit for bit now pin 1.4.0, since phrase starts changed on purpose. Every control keeps its AU parameter and ID. No new controls. Latency unchanged.

## 1.3.1, 2026-11-07

Renamed to Choir Loft, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.3.0

Up to 8 singers, and click to choose who you hear.

Voices 5 to 8, each with Interval, Level, Pan, Drift, Throat and Tone, and its own colour and card. Voices keeps its range of 1 to 4 and a new More Voices adds up to 4 on top, because a host stores automation of Voices as a position in its range, and widening it would have moved old automation. The window shows them as one row, 1 to 8.

Click a singer on the stage, or a voice's name on its card, to hear it or not. New parameters: Voice 1 to 8 On and Lead On. All brings everyone back. Off singers and their lines dim, fade over 15 ms and stop using CPU. A click with no movement switches, a drag moves. A double-click on a singer no longer resets it, because two quick clicks now mean off and on again.

Above 4 voices, the voices together turn down so 8 stay about as loud as 4: -3 dB at 8. New presets: Six Part Stack, Eight Part Choir, MIDI Eight Part Choir, after the old ones, which keep their places. No limiter.

Defaults and every old preset are bit for bit the same as 1.2.0. Sessions from before open with everyone on and their own voice count.

The window now says Choir Loft, Luke's final name. The build and AU name are unchanged for now.

## 1.2.0

The custom window. The display shows each voice as a line through time, running into its singer on a stage: up and down is the note, left and right is pan, size is Level. Drag a singer to pan it or change its interval. Voice strips with the note, the interval in words, Level, Pan, Drift and Tone. Everything else is behind Advanced. Replaces the generic window.

Every parameter keeps its ID, name and range. The sound is unchanged, bit for bit against 1.1.0 on 5 presets, with and without the window open. The processor now also stores a level per 5 ms for the display, lock free. New in saved state: window width and whether Advanced is open, nothing else.

## 1.1.0

More like separate people. Luke heard 1.0.0 on his voice and found it robotic. Nobody has heard this version yet.
New controls: Voice 1 to 4 Tone, Consonant Scatter, Breath. All old parameter IDs stay. Latency is the same.
New defaults: Drift 30, Humanize 50, Glide 30, Follow Lead 8, Consonants 22, wider Throat, per voice Tone. Presets updated. New preset Tight, for exact tuning within the 1.0.0 limits.
Humanize now also covers: settling off target, each voice's own tendency, standing behind the lead, a slower glide, softer starts, its own vibrato.
Each voice hears the lead late, so note changes do not land together. Drift also moves level a little.
Consonants can be each voice's own, later and softer, and breaths fade. A voice now sings to the end of the vowel it is reading.
With Humanize above 0, grains read between neighbouring periods, and read position is nudged per grain.

## 1.0.0

First version. Up to four formant preserving harmony voices from one lead vocal, with the lead delay matched.
Modes: Scale Interval, MIDI Chord, Fixed Interval, Chord Tones, MIDI Channel.
Controls: Mode, Key, Scale, Follow Key, Voices, per voice Interval, Level, Pan, Drift and Throat, Humanize, Vibrato, Vibrato Rate, Lead Level, Lead Pan, Harmony Level, Mix, Glide, Follow Lead, Consonants, Chord Root, Chord Type, Chord Hold, Input Type, Tracking, Reference A4, Bypass.
Readings: Detected Pitch, Lead Note, Voice 1 to 4 Note, Voices Sounding, Active Key. Ten factory presets. Generic window.
