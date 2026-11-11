# Changelog

## 1.5.0, 2026-11-11

Low notes get sung, and there is a Human control. Luke, on 1.4.4: "it misses low notes a lot and the voices sound robotic like really robotic. i mean i like the robotic sound but i also want a more human option". Both were measured on 11 of his own dry takes (Rode NT1, whole songs) with `realvox/RealVox.cpp`. Its checks and pass rules were committed before any change was measured (1f8e91e for the held notes, 772d144 for Human). The README section "Low notes and Human" has the tables.

- Low notes. Alto/Tenor, the default Input Type, tracked from 98 Hz, and Luke's low notes sit at 91 to 97 Hz. Of 28 held voice notes under 98 Hz, 1.4.4 sang 6 (12 missed, 8 dropped out part way, 2 on another note) and 1.5.0 sings 28. Time on the note there: 42.0% to 88.0%. All 22 failures had the one cause, the note was under the range. From 98 to 130 Hz: 697 of 700 to 700. Above 130 Hz: 2623 of 2630 to 2622.
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
