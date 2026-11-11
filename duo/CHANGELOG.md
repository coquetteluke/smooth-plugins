# Changelog

## 0.4.2, 2026-11-11

Luke said yes to this on 2026-11-11.

Wide Stack, Loose Gang and Two Singers now use Precise Vibrato, like the other presets and like a new Duo. Until now they kept the 0.3 vibrato law so that they would sound exactly as they did.

- What changes: these three presets set Vibrato Variation to 20, 40 and 30. With the 0.3 law the doubles are bent all the time by whatever the pitch does at about 4 to 9 Hz, vibrato or not, a little out of step. With Precise Vibrato they are bent only where there is a real vibrato, and then in step with it. On a voice with no vibrato, like Luke's on these takes, the doubles of these presets therefore move a little less against the lead than before. Measured on the 11 working takes: the level of each preset is the same within 0.05 dB, and what a pitch tracker after Duo gets is the same (0.10% of held-note frames off against 0.11% for Wide Stack, 0.50% against 0.46% for Loose Gang, 0.41% against 0.36% for Two Singers). On the 30 s clips of takes 09 and 16 the old and the new Wide Stack differ by 6 to 8 dB under the signal and Loose Gang by 2 to 4 dB under it, which is the doubles sitting a fraction of a millisecond elsewhere. Clips of the old and the new presets are in `_vox/rode/work/duo/demo/presets-main` and `demo/presets-precise-vibrato`.
- A session saved with one of these presets keeps the vibrato law it was saved with.
- The null test still holds every case to the 0.2.0 sound bit for bit, with Precise Vibrato switched off for it.
- Two test lines follow the presets: the line that held these three presets to "keeps the 0.3 vibrato it was made with" now holds every preset to Precise Vibrato on, and the old session test compares against Wide Stack with Precise Vibrato switched off. No limit changed.

## 0.4.1, 2026-11-11

Luke said yes to this on 2026-11-11.

A new Duo, and the Natural Pair preset, open with Match Level on. The default then comes out as loud as the dry vocal instead of 1.6 to 1.8 dB over it, so it no longer pushes whatever comes after it harder. Nothing else about the default changes: the same two doubles at the same balance, the same pans.

- On the 11 working takes the default is now within 0.16 dB (-0.16 to +0.05 dB, median -0.02 dB) of the dry over sung parts (it was +1.61 to +1.82 dB), and its peak is -7.5 dBFS (it was -5.7 dBFS).
- What it costs: at the default the lead is your vocal turned down by 1.8 dB, not your vocal sample for sample. Switch Match Level off and it is exact again.
- Sessions saved before keep Match Level off, so they sound and measure as they did. The other seven presets keep their level.
- The null test still holds every case to the 0.2.0 sound bit for bit, with Match Level switched off for it.

## 0.4.0, 2026-11-11

Three new switches in Advanced. With all three as an older session loads them, the sound is bit for bit what it was (the null test still passes for every preset).

- **Precise Vibrato**, on in a new Duo. Vibrato Variation was built around one vibrato speed and one distance: it only sang double 1 almost straight when your vibrato was near 5.5 Hz and the double was about 25 ms behind. Measured on Luke's takes with a known vibrato put onto the held notes (he sings almost none of his own: 3 of the 324 stretches the first check called vibrato hold one, the rest are slides and wobble), double 1 kept 0.52 of a 4.5 Hz vibrato, 0.47 of a 6.8 Hz one, and 0.70 at Tightness 85, against a limit of 0.5. Now Duo fits your vibrato's own speed and reads it at the moment each double is playing: double 1 keeps 0.18 to 0.29 at every speed and Tightness tried, double 2 sings 1.41 to 1.44 times as wide, double 3 0.56 times. Slides and note changes are left alone (the bend added on the slide test went from 33 cents to 0).
  Sessions saved before 0.4.0 load with it off and sound exactly as they did. Wide Stack, Loose Gang and Two Singers, the presets that use Vibrato, keep it off for the same reason. Switch it on to hear the fix.
- **Mono Safe**, off unless you switch it on. Each double goes to the left and the right with opposite signs, so adding left and right leaves your own voice alone (what is left of the doubles is under -150 dB). A phone speaker then plays the lead only, and a plug-in after Duo that listens to left plus right, as Choir Loft does, tracks a clean voice instead of a doubled one. In stereo the doubles are all there at the same level, wide and not placed. Width and Doubles Pan are dimmed while it is on.
- **Match Level**, off unless you switch it on. Duo adds voices, so it is louder than what goes in: 1.6 to 1.8 dB at the default. Match Level turns the lead and the doubles down together by what they add up to, so the output is as loud as the dry vocal (within 0.2 dB on Luke's takes, on the default and on the widest presets). The balance between you and the doubles stays. With it on the lead is the input turned down, not the input sample for sample.

Checked on the 11 working takes of the Rode session. The small steps the shared click check counted as Duo's own (13) are all sounds of the take itself, copied by a double: with the take low passed so that it holds nothing sharp, every one is gone and nothing above 8 kHz comes out (-76 dB), and a 9.6 kHz sine comes out clean for the whole length of every take (-105 dB). Timbre Variation at 100 does not move the pitch: the harmonics stay within 0.8 cents; what the pitch reader saw 3 semitones away were breaths and consonants whose resonance Timbre is meant to move.

## 0.3.1, 2026-11-07

Renamed to Duo, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0

New window: the stage from straight above, in slate and sky blue. Left to right is pan, and further back is later behind the lead, read live from the sound. Each double has a tape mark and a lane showing how far it can drift, its shoulders turn with its pitch drift, and a pool of light is as big as what it is singing. Drag a double sideways for Width or back for Tightness, drag the lead for Lead Pan. Width sits under the stage with a handle under each double, Tightness runs up its side, plus level faders, the note being followed and each double's live delay. Advanced holds Timing, Pitch, Vibrato, Timbre, Vocal Range, both pans and Extra Delay. Resizable, with the size and open section saved in the session. Audio is bit identical to 0.2.0, proven by a null test on real speech. The processor gained one read-only input peak for the window.

## 0.2.0

First installed version. Vocal doubling with 1 to 3 copies, Tightness, Width, Lead Level and a level per double, plus Timing Amount, Pitch Amount, Vibrato Variation, Timbre Variation, Vocal Range, Lead Pan, Doubles Pan and Extra Delay. Eight factory presets. Zero latency.
