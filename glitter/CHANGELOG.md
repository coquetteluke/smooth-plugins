# Changelog

## 1.3.0, 2026-11-17

Switchboard can now see and set every control through the link. Sound unchanged.

`src/SmoothLink.h` is a byte copy of Switchboard's: the plug-in keeps a card of every control in the shared link folder and moves its own parameter, with a gesture as a mouse drag does, when Switchboard sends a set; the card carries the track name Logic gives it. Main controls: Retune Speed, Humanize, Output Gain, Scale, Follow Key, Bypass (the biggest controls always in the window). New test `PluginTest link`: the card lists all 80 parameters by ID, "60 ms" on Retune Speed lands and is confirmed, closing removes the card. No DSP, parameter, ID, range, default, preset, latency or AU code changed; the render sums of the default and all 12 presets equal 1.2.5's.

## 1.2.5, 2026-11-13

The Phase 2 bug check: the 28 faults the hunt kept, HostStress's two paramtext FAILs and the key file items, each with a check written first that fails on 1.2.4 (`PluginTest phase2`: 46 failing on 1.2.4, 0 now). Every parameter, ID and range is as before, so saved sessions load, and sessions saved by 1.2.4 play exactly what the installed 1.2.4 played (default and eight presets, -240 dB). Luke left this call to the lead on 2026-11-11: the sound only changes where it was at fault, listed below.

Round 1's real vocal checks were run again on 1.2.5 before it was installed (working set and all 12 presets, same check file): every rule and every number reads as on 1.2.4 (120 rules, the same 36 failing; the presets' rules the same), CPU aside. Those checks render in stereo, so the mono change below does not show there.

Sound:
- Reverb switched on when a block starts part way into the reverb's 16 sample step (once a single odd sized block had played, at a cycle wrap say, that was every block) stayed silent until the plug-in was prepared again. It now wakes: RT60 1.72 to 1.76 s at Decay 2 s.
- No voice could move more than two octaves from what was sung: Pitch -2 octaves left a sharp note sharp (30 cents on a note sung 30 cents sharp, now 0.002), Harmony -24 did the same, and Pitch +2 octaves +7 played +2 octaves exactly, out of key. The grains now reach 2.5 octaves, so the correction fits on top of two octaves, and a setting past two octaves drops by octaves into reach and keeps its note (+2 octaves +7 plays E5 from A3).
- Reverb Pre-Delay, Doubler Timing and a new Filter Sweep Rate no longer jump. Pre-Delay and Timing crossfade to their new read point over 30 ms, as Delay Time does: a Pre-Delay jump stepped 0.271 straight into the wet signal (0.013 now, steady 0.012), and a Timing jump bent the doubles further than the check can read, 2.4 semitones and more (about 20 by the arithmetic; now 20 cents at most, the doubles' own wander). Sweep Depth glides like Cutoff, and a new Sweep Rate glides from where the old one had the cutoff: 4 and 1.2 octaves in 16 samples before, 0.07 and 0.02 now.
- Preset arrows and the host's preset menu write each control once, with its final value. Every control used to pass through its default first, so a block could play all defaults while a preset loaded (a module on in both presets switched off, Delay Time jumped to 1/4, Output Gain to 0 dB).
- Input Type changed with the Doubler on no longer clicks in the doubles 16 and 22 ms later (a step of 0.252, now 0.007).
- Mono in and out: the doubles and two Harmony voices are no longer folded down at half level when panned, so Width no longer sets their loudness (6.0 and 4.4 dB). On a mono track Wide Double, Chipmunk Hook and Power Harmony's added voices are louder by that much; stereo is unchanged.
- A Harmony Interval changed in a pause moves the added voice straight away, so the next phrase starts on it instead of sliding (4.4 semitones off in its first 10 ms).
- The host's Reset clears delay and reverb tails, MIDI waiting in the latency and held notes, as preparing does, with no allocation. The Reset that comes with the host's bypass switching on is left out, so the bypass fade stays.

Key file (Key 1.7.2):
- Follow Key reads the key and major or minor in one load, so a file written in between can never give a key neither file holds.
- In an offline bounce or freeze the key file is read every 0.3 s of audio, so a Key change lands at its song position (1.2.4 read it every 0.3 s of wall time).
- A Manual key (written with listening false and rewritten at least every 240 s) and a Locked key from a Key that has closed are followed, as before. Checks guard both.

Host:
- The readings Target Note and Active Key read their own text back ("no note" was read as C-1, "B Custom" as C Chromatic). HostStress paramtext: 2 FAILs before, none now.
- The chosen preset is saved with the session. A session from 1.2.4, or one with no preset chosen, says "Choose a preset" instead of Hard Tune, and the arrows go to the first or last preset from there.

Window:
- The header says Glitter.
- With Advanced open the module picture underneath said it moved on every frame, so the window never dropped to its idle timer. Advanced now glides open and shut over a fifth of a second (it jumped 98% at the click), and never covers the footer while it moves.
- The pitch strip clears its last trace and shows its hint again once the voice has scrolled off, places its picture by the count it drew, and keeps the voice coming out on screen when a Harmony voice is two octaves away.
- The Harmony picture names the note sung now, not one last seen on Motion's panel. The correction bar shows nothing while bypassed. The header says Following only when a key from the file is in use.
- "Advanced (2)" replaces "Advanced, 2 changed", which lost its last word, and the hint line stops short of the latency reading.
- On Band Pass the Resonance dot stays on or under the band's drawn peak. Typing "2.5k Hz" gives 2500 Hz (it gave 20).

## 1.2.4, 2026-11-12

Two fixes from the second pass over Luke's real takes. Luke left this call to the lead on 2026-11-11. Every parameter, ID and range is as before, so saved sessions load.

- Pitch an octave down (and Harmony an octave down, and Octave Monster) moved the voice's formants down by 6.25 to 6.5% on Luke's takes, over the real vocal check's 4.43%: the grains' reading speed, which corrects for the formants a shift drags along, kept growing all the way down, where on a real voice the grains barely drag them at all. It now stops growing past 3 semitones, as it already did upwards: 1.25 to 1.5% on the same takes. The test vowels read 1.013 and 1.009 an octave down (0.963 before). Shifts within 3 semitones, and the lead, are unchanged.
- The tracker read a clean voice an octave low, for about 20 ms at a time, where every other period differs a little (Luke's take 01 at 200 to 226 Hz): its guard against a strong second harmonic moved to twice the period although both fit almost perfectly. The lead never let it be heard, but Pitch, Harmony and the Vocoder took the reading and jumped an octave. The guard now also needs a clear difference: Luke's take 01 at 8.42, 12.54 and 71.72 s is read at its pitch (1.2.3: an octave low for 19 to 23 ms), and so is a held voice whose every other period is 20% weaker (1.2.3: an octave low throughout).

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
