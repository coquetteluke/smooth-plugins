# Changelog

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
