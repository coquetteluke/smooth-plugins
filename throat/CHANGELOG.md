# Changelog

## 0.5.0, 2026-11-11

Luke said yes to this on 2026-11-11, after the clips in _vox/rode/work/throat/demo.

Three changes to how every setting but Neutral sounds. Throat's own tests pass. NullTest held the sound to the 0.2.0 renders; with Luke's yes its renders were saved again from 0.5.0 (the 0.2.0 renders stay in git history, and the saved 0.2.0 session file is kept as it was, so a 0.2.0 session is still loaded and compared). Against the 0.2.0 renders, 0.5.0 nulls exactly at the default (Neutral) and differs at Big Chest -20.0, Giant -25.7, Pinched -26.2, the 0.2.0 session -28.6, Subtle Blend -45.9 and Whisper -64.3 dBFS. On the whole working set of real takes the failing rules went from 436 to 329: whistling passes at every setting (Width 2.00 9.96% to 0.35%, limit 1%), the hiss rule at Length 1.30 0.62 to 0.02% and Length 1.50 2.24 to 0.32% (limit 0.5%), and the first 30 ms of a phrase pass at the median at all 9 settings that failed. One rule got worse: formant scatter above 200 Hz at Length 1.50, 5.375 to 5.75% (limit 5.5).

- Phrase starts. Level Match's slow step kept the last phrase's correction into the next one, and the first 30 ms of a phrase came out about 2 dB quieter at settings that lower the formants. Here a phrase that starts after a pause gets its own average.
- Whistling. A harmonic that landed on a narrow peak of the model came out far above its neighbours (1 to 10% of voiced frames on real vocals). Each harmonic from 500 Hz up is now held to standing out by no more than 8 dB, or 2 dB more than the input's most prominent one. The guard reads what a harmonic really gets, with the bins beside it that the short window spreads it over, and it also holds on frames that are voiced but less clearly so.
- Hiss bursts, the rest of them. 0.4.1 holds the gain above 6 kHz to 11 dB over what moving the envelope itself would give, the least that leaves the 0.2.0 renders bit for bit. Length 1.50 still had 1.8% of voiced frames more than 12 dB over the tube model from 6 to 12 kHz on takes 01 and 16, mostly where the level match lifts the whole frame. Here it is 8 dB: 0.16%.

## 0.4.1, 2026-11-11

From the real vocal re-test on the Rode takes. Only frames that had the fault below sound different: the 0.2.0 renders of every preset tested still null bit for bit.

- Fixed: a burst of hiss on some sung vowels at settings that lengthen the tract (Length over 1, A Little Longer, Width 2.00). Everything from about 6 kHz up came up by 20 to 26 dB for 50 ms or more (take 01, 34.5 s and 54.5 s, Length 1.20). The formant model, an all-pole fit up to 8 kHz, could put a peak at the top of its band where no harmonic answers to it, up to 36 dB over the voice's envelope, and the map turned that into gain for the whole top of the spectrum. Above 6 kHz the gain is now held to at most 11 dB over what moving the envelope itself would give. On takes 01 and 16, frames more than 12 dB over the tube model from 6 to 12 kHz: Length 1.20 1.64% to 0%, A Little Longer 1.11% to 0%, Width 2.00 0.53% to 0%, Length 1.50 3.71% to 1.80%. New test: a steady tone with that vowel's harmonic levels, Length 1.20, 6 to 12 kHz over the envelope moved the same way 16.4 dB before, 7.3 dB now (limit 12).

## 0.4.0, 2026-11-11

From the real vocal re-test on the Rode takes. A still setting sounds exactly as in 0.3.1, sample for sample: the default, every preset, and saved sessions.

- Fixed: a host that switches its own bypass on under audio got 32 ms of silence and a click, and the engine started again from nothing. The AU wrapper resets the plug-in in the same call that sets Bypass, and that reset emptied the delay line. A reset that comes right behind Bypass going on is now left out, so the host's bypass fades like the Bypass control does. Every other reset clears as before.
- Fixed: choosing a preset or moving a control while audio plays let the level jump for about a tenth of a second (up to 8 dB on a real vocal), because the gain moved in 40 ms and the slow part of the level match still held the old setting's two second average. While a control is moving the level match now follows frame by frame.
- New: Peak Guard, in Advanced, off by default. Moving formants changes the shape of the wave, so peaks can come out higher than they went in at the same level (up to 3 dB over full scale on a take that peaks just under it). On, the output never peaks higher than the input has. No preset changes it, and off it adds nothing.

## 0.3.1, 2026-11-07

Renamed to Throat, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0

Its own window. A side view of the vocal tract as one bent tube from the cords to the lips, with the singer's throat in bone and the model in oxide red, both read live from the voice's formants. Five draggable points, a gray realistic range for width and length, Reset, a level meter, F1 to F5 and pitch before and after, the two formant curves, and a quiet latency readout. Sound is unchanged: output nulls bit for bit against 0.2.0 renders on speech, with the window closed, drawing, and being reopened during audio. Sessions from 0.2.0 load as before. The only additions to a saved session are the window's size and whether Advanced is open.

## 0.2.0

First finished version, matched from the start to the control set of the plug-in it replaces. Throat Length, Throat Width, five points along the tract each with its own width, four of them with a position, Breathiness and Breath High Pass, Source and Model Voice Type, Glottal Pulse Width, Vocal Range, Throat Precision, Mix, Output Gain, Level Match, Bypass, and a read-only Output Level. Fourteen presets. Latency 32 ms at 48 and 96 kHz, 34.8 ms at 44.1 and 88.2 kHz, dry path delay-matched. The formant engine lives in src/formant so other plug-ins can copy it.
