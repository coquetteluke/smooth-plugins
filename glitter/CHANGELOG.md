# Changelog

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
