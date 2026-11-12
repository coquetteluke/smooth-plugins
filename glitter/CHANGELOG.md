# Changelog

## 1.2.3, 2026-11-12

Vocoder: 1.2.0 to 1.2.2 kept a second level match for consonants and the room. On a room with a rumble under the vocoder's lowest band it grew far too big, and where the voice left its voicing it took over: Robot Stack on Luke's take 06 reached 0 dBFS where the voice was at -42 dBFS (20.9 dB over the voice nearby, now 4.8). Back to one level match, learnt on voiced sound only, which also carries the consonants. The first note and switch-on fixes of 1.2.0 to 1.2.2 stay. Luke left this call to the lead on 2026-11-11.

## 1.2.2, 2026-11-12

Vocoder: 1.2.0's level match could put the first note after a pause or a switch-on far over its level (up to 30 dB for its first 10 ms; Robot Stack on Luke's take 06 peaked at +2.6 dBFS on the first note): its first reading was taken while the filters were still ringing up, and a gain learnt on a room's noise stood in for the voiced one. A first reading now waits until the carrier has played 20 ms over sound, nothing stands in, and the tone comes in over a few ms: 1.34 dB at most. Its steady sound is unchanged. Luke left this call to the lead on 2026-11-11.

## 1.2.1, 2026-11-12

Distortion switched on in the middle of a note came in up to 7 dB high or low for a moment, until its new level hold had caught up (on Luke's takes one switch-on step read 1.53 times the voice's own, against 1.5). The hold now reads the level quickly for the first 20 ms of sound after the switch, under the switch's own fade: 0.33 dB at most. Luke left this call to the lead on 2026-11-11.

## 1.2.0, 2026-11-12

Fixes from Luke's real takes (Rode NT1, 17 songs, 2026-11-10). Luke left this call to the lead on 2026-11-11: the tuner, Vocoder, Distortion and two presets sound different where they were at fault. Every parameter, ID and range is as before, so saved sessions load.

- The tuner has the fixes Tune Pro found on these takes, as Tune 1.2.0 has them (Glitter's tuner is the same copy of Smooth Tune). Alto/Tenor follows notes down to 70 Hz at the same latency: held notes at 78 to 92 Hz were left 35 cents off, now 0.004. The pitch control is in line with the audio it belongs to: it ran up to 13.5 ms early, now 0.22 ms. A voice that is not corrected comes out as the input at the reported delay (it was up to 5.6 dB off it, now exact). A note held on the line between two notes keeps its target through a waver: 62.6 cents of waver came out, now 0.19. Nothing sounds before a note starts (-20.6 dB to -137.5 dB). On the takes, at the song's key: the worst take's held notes 24.2 to 2.2 cents (take 02, low notes), notes left uncorrected 4 to 1.
- Reverb switched on while playing (automation, or a click on its block) never decayed and built up 8.6 to 16.4 dB over the voice: clearing the tank wiped the loss that makes it decay. Now RT60 0.84 and 2.60 s for Decay 1 and 3 s.
- Vocoder: a note after an S or a pause came in up to 7.8 dB over its own level, and the tone could come in about 13 dB over its own level when switched on. Two level matches now, one for voiced sound and one for consonants and the room, each starting at its first reading, and the tone fades in over 10 ms when switched on. Its steady sound is unchanged.
- Distortion holds the level of the voice going in (slowly, 0.3 s): its make-up suited a -18 dBFS voice, and a quieter one came out up to 6.8 dB louder (Luke's takes at the default Drive: 3.5 dB). The curve, Drive and Tone are as before.
- Phone Line and Octave Monster carry the old Distortion make-up in their Output Gain (10.8 and 1.9 dB), so they are as loud as before.
- Suite numbers that moved, all within their limits: Retune 25 to 200 ms measures 28 to 203 ms (22 to 197), steady tones held off a correction read 1.73 cents (0.003) while the grains walk back to the reported delay.

## 1.1.1, 2026-11-07

Renamed to Glitter, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

The custom window. The live pitch strip with your voice, the output and Harmony's voices, then a bold row of eleven colour blocks in signal order, lit when on, and one panel for the selected block with a picture of its real settings and all its controls. Most pictures are pads that move two controls together. Advanced holds Input Type, MIDI Target, Reference A4, Tracking and a keyboard for your own scale. Double-click resets, click a value to type it, the window resizes and remembers its size, Advanced and the selected module.

No sound changes. All 12 preset renders are byte for byte the same as 1.0.0, and all on with the window open and drawing against no window is exact zero. All parameters keep their IDs, names and ranges.

Window CPU, real window at 2x, 30 frames a second, speech playing, other builds running on the machine. CPU time depends on which cores macOS picks, so work in fast core terms is beside each run.

| Panel showing | CPU, % of one core | Work, % of a fast core | Share on fast cores |
|---|---|---|---|
| Filter with its sweep moving, the busiest | 5.1, 16.3, 12.4, 11.7, 7.4 | 4.2, 4.4, 4.4, 4.0, 3.8 | 89, 9, 41, 45, 64 |
| Pitch, nothing moving in the panel | 5.0, 4.6, 4.7 | 2.9, 3.0, 2.9 | 92, 94, 96 |
| No audio, nothing moving | 0.9, 0.8 | 0.3, 0.3 | 84, 85 |

The higher Filter runs are macOS putting it on the slow cores: the audio thread read 9.4% in the worst one against 2.4% on fast cores, for the same work. Audio CPU through the real AU: 1.9% before the window, 1.8% after.

## 1.0.0

First version. Fast creative vocal tuner with held MIDI target notes and eleven switchable modules in a fixed order: Pitch, Throat, Motion, Harmony, Doubler, Vocoder, Distortion, Filter, Ring, Delay, Reverb.
Tuning controls: Key, Scale, Retune Speed, Humanize, Input Type, Follow Key, MIDI Target, Reference A4, Tracking, Custom notes. Output Gain, Bypass.
Readings: Pitch Off Target, Detected Pitch, Correction Applied, Target Note, Active Key, MIDI Notes Held. Twelve factory presets. Generic window plus a live reading strip.
