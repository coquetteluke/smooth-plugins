# Changelog

## 1.1.0

Quality work. High Quality numbers, worst case on the test voices, each step measured against the one before. Flat means the first formant reads flatter than a real voice.

| Step | Flat | Formant move | Level | Other |
|---|---|---|---|---|
| Version 1, plain grains | 11.6 dB | 4.98% | 1.15 dB | big shifts 15.7% and 10.2 dB, high notes 45% |
| Take the vocal tract out before grains and put it back after | 9.9 dB | 4.77% | 5.1 dB | big shifts 7.1% and 5.6 dB, high notes 21% |
| Narrow the peaks that get put back, 25% | 8.5 dB | 4.98% | 4.0 dB | big shifts flat 2.8 dB |
| Centre grains on the pulses | 7.6 dB | 4.98% | 4.2 dB | non-harmonic energy needed a slower offset, below |
| Slower pitch mark slide, from Smooth Harmony | 7.5 dB | 4.56% | 4.2 dB | worst output pitch 2.98 to 0.80 cents |
| Draw the model through harmonic peaks on high voices | 7.8 dB | 5.6% | 2.0 dB | colour 3.3 to 2.8 dB, level with a slow trim |
| Sharpening set per frequency, 45 to 50% | 6.9 dB | 4.98% | 1.45 dB | |
| Hold the pulse offset for the whole note, not re-found | 6.8 dB | 4.98% | 1.45 dB | non-harmonic energy -14.7 to -46 dB where it had broken |
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
