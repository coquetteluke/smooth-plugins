# Changelog

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
