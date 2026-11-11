# Changelog

## 0.4.0, 2026-11-11

Real vocal pass, on Luke's Rode takes. Two faults fixed, one new switch, three new presets. With Clean off, every preset and every setting that is not being moved sounds exactly as in 0.3.1, sample for sample.

Fixed:

- A control that was moved stopped short of where it was put. Delay Time could stay up to 0.0003 ms off for good, and Voices, Depth, Low Cut, High Cut and Saturation likewise. Against the same setting held from the start that was a difference 55 dB under the sound on the takes (37 dB in the tests at 48 kHz, 24 dB at 96 kHz). Every control now lands exactly.
- Bypass ticked on a steady tone. The 20 ms fade was a straight line with a corner at each end, which left a tick above 8 kHz, 78 to 86 dB under a 1 kHz tone. The fade is now S shaped: 105 to 132 dB under.

New:

- **Clean**, in Advanced under Saturation. Off by default, and off is the 0.3.1 sound. On, it does four things that could not be done without changing that sound:
  - Nothing folds back from Saturation. A 5 kHz tone at Saturation 100 left a line at 23 kHz, 52 dB under the tone. With Clean it is 119 dB under. This was the first limit of the fold back test (70 dB), which the first build could not meet and turned into a reading.
  - Saturation no longer lifts quiet sound. The standard curve keeps a -12 dBFS tone at its level, so anything quieter comes up: 3.6 dB at Saturation 100, 1.2 dB at 50, and S sounds with it. With Clean, quiet sound keeps its level exactly and only the peaks are rounded off.
  - Wow and Flutter lose their hash. Their irregular part left noise around a held note, 42 dB under a 3 kHz tone at the default Wow 15 and Flutter 10 and 23 dB under with both at 100. With Clean it is 69 and 49 dB under, and they still swing as far (2.25 ms and 0.23 ms at 100).
  - Low notes are on time. The DC filter in the voices sits at 1 Hz in place of 6 Hz, where it brought a 100 Hz note in 4.6 samples early.
  The delay you set is still the delay you get, to 0.002 sample at every rate, and latency is still 0. Switching it under sound fades over 30 ms with no tick (110 dB under a tone).
- Three presets, after the 13 old ones so their numbers do not move: **Clean Vocal Double**, **Clean Wide Stack**, and **Worn Cassette Clean** (Worn Cassette control for control, with Clean on).

Tests: limits that were wider than what the README says are now what it says. Mix 0 and Bypass must match the input in every sample (the limit was 120 dB under). Delay Time is held to 0.02 ms (it was 1 ms), and to 0.05 sample for one still voice at 44.1, 48, 88.2 and 96 kHz. The click limit of the shared test kit was a step of 0.30 on a tone whose own step is 0.004, and is 0.012 here. Nothing was hiding behind those three. New tests for arrival, the Bypass tick and Clean each failed before their fix.

## 0.3.1, 2026-11-07

Renamed to Tape Chorus, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0

Its own window, in place of the generic one. Dark sea green and pale mint. Two reels with one line per voice between them, showing each read head's real delay moving over time, left and right in two colours, over bars of what is really on the tape. The left reel winds out with the input level and the right with the output. Worn tape shows as gaps in the lines, and Feedback as repeats in the bars. With no audio running it draws what the settings will do, and it stops drawing when nothing moves.

Drag anything, double-click to reset, click a number to type. Drag the tape for Rate and Depth, and a reel for its gain. Advanced holds Wow, Flutter, Random, Unsync, Saturation, Degrade, Noise, Noise Gate, Tone, Low Cut and High Cut.

The sound is the same: through the real AU, 19 renders against installed 0.2.1 at 44.1, 48 and 96 kHz, in stereo and mono, every preset and all controls moved, are bit for bit identical. Parameter IDs, names, ranges, defaults, presets, latency and tail are unchanged. A session gains only the window size and whether Advanced is open.

## 0.2.1

CPU work only. The sound is the same: renders of the default, every preset and random automation at 44.1, 48 and 96 kHz and in mono null against 0.2.0 at -114 dBFS or better, which is the last bit of the 32 bit output. Parameter IDs, defaults, presets, latency and tail are unchanged.

The oversampling filters now slide their history once per piece of audio and work out four outputs at a time. The parts of the saturation curve that depend only on Saturation are worked out once while it holds still.

## 0.2.0

Second pass, checked against the public manual of the chorus in UVI Tape Suite. The first 7 presets and any 0.1.0 session sound exactly as before, measured sample for sample.

Added: up to 8 voices, Feedback, Input Gain, Output Gain, Low Cut, High Cut, Wow Rate, Flutter Rate, Random, Random Rate, Unsync, Degrade, Degrade Rate, Noise, Noise Gate. Readings: Input Level, Output Level, Pitch Swing. Six new presets.

Fixed: after a visit to more voices, modulation at short Delay Times stayed smaller than it should. Voice count changes now glide more gently.

Voices now runs 1 to 8 instead of 1 to 4. Saved values are kept, but Voices automation already written in a session would now land on a different count.

## 0.1.0

First version. Tape style chorus with 1 to 4 voices, Rate, Depth, Delay Time, Wow, Flutter, Saturation, Tone, Spread, Mix and Bypass. Seven factory presets. Zero latency.
