# Changelog

## 0.5.0, 2026-11-11

Second real vocal pass, on Luke's Rode takes. Clean becomes the default, four old presets come inside the limits, Feedback near the top comes down, and a corner at the end of a Mix move is gone. Every saved session sounds as it did, except at Feedback 90 and with Clean on at Saturation over 60.

Changed (sound):

- **Clean is on in a new instance and in every factory preset.** Luke left this call to the lead on 2026-11-11. At the default this removes what the real vocal checks found with Clean off: Saturation 100 lifting the voice 3.3 to 3.7 dB and S sounds 3.3 to 3.6 dB, fold back at 20 to 24 kHz of -51.9 dB (limit -70), hash from Wow and Flutter of -41.6 dB (limit -50), one still voice 0.13 to 0.43 sample early on the four lowest takes (limit 0.1). A session keeps the Clean it was saved with, and a session saved before Clean existed (0.3.1 and older, with no Clean in it) opens with Clean off, so it sounds exactly as it did. The test for that was written first and failed: with the new default and no rule for old sessions, a 0.3.1 session opened with Clean on and rendered up to 0.258 away from itself.
- **Four old presets brought inside the limits.** Luke left this call to the lead on 2026-11-11. Measured with Clean on, on all 11 working takes, the smallest change that meets each limit:
  - Worn Cassette: Spread 50 to 27. Mono, worst voiced quarter second -9.55 to -5.82 dB (limit -6; 28 gave -6.24). Its peak over the dry, 3.24 dB, was already 2.59 with Clean on, now 1.75.
  - Cassette Backing: Spread 100 to 71. Mono worst quarter second -8.07 to -5.91 dB (72 gave -6.10).
  - Fast Shimmer: Spread 80 to 69. Left against right level 1.14 to 0.99 dB (limit 1; 70 gave 1.00 over, and less Spread raises the peak, 3.13 dB at 65). Peak over the dry 2.91 dB.
  - Resonant Tape Swirl: Mix 40 to 39. Peak over the dry 3.004 to 2.93 dB (limit 3; 3.11 with Clean off).
  - Wide Four Voices needed nothing: its peak over the dry was 3.04 dB with Clean off and is 2.90 with Clean on.
- **Feedback near the top comes down.** Luke left this call to the lead on 2026-11-11. At Feedback 90 the peaks of a voice read 3.2 to 3.8 dB over the dry on takes 05, 09 and 10 (limit 3). From 70 up, what goes onto the tape is split at 1 kHz: at 90 the band under it comes down 2 dB and the band over it comes up 0.3 dB, smoothly from nothing at 70. Feedback 70 and below renders 0.4.0's samples exactly (checksums in the tests). A plain 1 dB cut was not enough on take 05 (3.06 dB) and pushed white noise at Feedback 90 past its own test limit (2 dB under Feedback 0), because the loop already darkens: white noise read 1.9 dB under while dark noise read 0.4 dB over. Now on all 11 takes the peak at Feedback 90 is 2.69 dB at most with Clean on (was 3.63) and, with Clean off, 2.90, 2.61 and 2.16 dB on takes 05, 09 and 10 (were 3.77, 3.79, 3.20). Dark noise comes down 0.85 dB, white noise stays where it was (1.86 dB under Feedback 0). A session saved at Feedback above 70 changes with it; that is the fault being fixed.
- **Clean on meets the README's Saturation claim.** Clean's drive bends over above Saturation 60, to 5.0 at 100 in place of 6.0, so a -12 dBFS tone comes down 2.82 dB at Saturation 100 (limit 3; 0.4.0's Clean took 3.53). Saturation 60 and below is unchanged, and so is every preset.

Fixed:

- A Mix move ended with a corner. Mix glided in a straight line (its whole range in 20 ms) with a corner at each end, the fault Bypass had until 0.4.0: on a steady 1 kHz tone a move left a tick above 8 kHz 70 to 77 dB under the tone. It now glides in two stages, as a voice fades: 91 to 99 dB under (limit 90). This is what the blind set found on take 11 (one control step of the realvox tone twin missed, in 0.4.0 and 0.3.1 alike): reproduced on working take 07 with take 11's step positions, the step fell 6 samples after that corner, 7.4 ms after a move where Mix went from 37.2 to 74.3 %; holding Mix still at that move made it found. The test, on the twin's own tone and rule, failed first and passes now.

Tests: written first and failing on 0.4.0 (14 failures), then passing. Clean on now has its own twin of three tests that hold the standard path, each with the same limit: Saturation 100 keeps a -12 dBFS tone within 3 dB (passes, -2.82 dB); the noise level at Mix 25 to 100 within 1 dB, on white noise low-passed at 20 kHz, because Clean removes everything above 20 kHz by design and full-band white noise, a sixth of whose power sits there, reads 1.1 dB lower (passes, -0.27 dB at Mix 100; the full-band number is written beside it); and Spread 100 correlation of dark noise under 0.3 at 3 to 8 voices, which Clean on cannot meet at 4, 5 and 7 voices (0.304, 0.301, 0.331) together with the low-note timing it exists for. Measured at 7 voices against the worst low-note timing on Luke's takes (limit 0.1 sample): Clean's DC filter at 1 Hz 0.331 and 0.071; 2 Hz 0.323 and 0.142; 2 Hz in two stages 0.314 and 0.285; 3 Hz in two stages 0.298; 6 Hz (standard) 0.282 and 0.427. No filter can take 1 to 6 Hz out without bringing low notes in early, and no pan layout helps (5 and 7 voices have a centre voice, and moving it breaks left against right balance, 1.34 dB at pan 0.5, limit 1). Luke left this call to the lead on 2026-11-11: Clean keeps its 1 Hz filter, those three are written as readings beside the Clean-off test, which keeps its 0.3, and the README says so. NullTest's preset cases are now the 0.3.1 preset values with Clean off, still bit for bit against 0.2.1. New `tests/au_session_null.sh` (with `savestate=` and `loadstate=` in `tests/AuRender.cpp`): states saved by the kept 0.3.1 and 0.4.0 builds, rendered by them and opened in 0.5.0 through the real AU, 20 of 21 and 25 of 27 identical, the rest on purpose (Feedback 90; Clean at Saturation 100). `TAPE_CHORUS_ONLY=<test,...>` runs single tests.

## 0.4.0, 2026-11-11

Real vocal pass, on Luke's Rode takes. Three faults fixed, one new switch, three new presets. With Clean off, every preset and every setting that is not being moved sounds exactly as in 0.3.1, sample for sample.

Fixed:

- A control that was moved stopped short of where it was put. Delay Time could stay up to 0.0003 ms off for good, and Voices, Depth, Low Cut, High Cut and Saturation likewise. Against the same setting held from the start that was a difference 55 dB under the sound on the takes (37 dB in the tests at 48 kHz, 24 dB at 96 kHz). Every control now lands exactly.
- Moving Low Cut or High Cut put a small staircase on the voices: while a cut glided, its filter took the new setting once every 32 samples and the sound stepped each time (a bend 2.6 times the steady one on a 100 Hz tone as Low Cut went from 150 to 600 Hz). The filters now glide sample by sample: 1.01 times.
- Bypass ticked on a steady tone. The 20 ms fade was a straight line with a corner at each end, which left a tick above 8 kHz, 78 to 86 dB under a 1 kHz tone. The fade is now S shaped: 105 to 132 dB under.

New:

- **Clean**, in Advanced under Saturation. Off by default, and off is the 0.3.1 sound. On, it does four things that could not be done without changing that sound:
  - Nothing folds back from Saturation. A 5 kHz tone at Saturation 100 left a line at 23 kHz, 52 dB under the tone. With Clean it is 119 dB under. This was the first limit of the fold back test (70 dB), which the first build could not meet and turned into a reading.
  - Saturation no longer lifts quiet sound. The standard curve keeps a -12 dBFS tone at its level, so anything quieter comes up: 3.6 dB at Saturation 100, 1.2 dB at 50, and S sounds with it. With Clean, quiet sound keeps its level exactly and only the peaks are rounded off.
  - Wow and Flutter lose their hash. Their irregular part left noise around a held note, 42 dB under a 3 kHz tone at the default Wow 15 and Flutter 10 and 23 dB under with both at 100. With Clean it is 69 and 49 dB under, and they still swing as far (2.25 ms and 0.23 ms at 100).
  - Low notes are on time. The DC filter in the voices sits at 1 Hz in place of 6 Hz, where it brought a 100 Hz note in 4.6 samples early.
  The delay you set is still the delay you get, to 0.002 sample at every rate, and latency is still 0. Switching it under sound fades over 30 ms with no tick (110 dB under a tone).
- Three presets, after the 13 old ones so their numbers do not move: **Clean Vocal Double**, **Clean Wide Stack**, and **Worn Cassette Clean** (Worn Cassette with Clean on, and Spread at 20 in place of 50 so that it holds up in mono).

Tests: limits that were wider than what the README says are now what it says. Mix 0 and Bypass must match the input in every sample (the limit was 120 dB under). Delay Time is held to 0.02 ms (it was 1 ms), and to 0.05 sample for one still voice at 44.1, 48, 88.2 and 96 kHz. The click limit of the shared test kit was a step of 0.30 on a tone whose own step is 0.004, and is 0.012 here. Nothing was hiding behind those three. New tests for arrival, the Bypass tick, the cut staircase and Clean each failed before their fix.

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
