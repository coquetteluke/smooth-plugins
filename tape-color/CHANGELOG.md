# Changelog

## 0.5.0, 2026-11-17

Switchboard can now see and set every control through the link. Sound unchanged.

- The link is `src/SmoothLink.h`, a byte copy of Switchboard's. It puts a card of every parameter and its value in the shared link folder, and applies set commands by moving the real parameter with a begin and end gesture, as a drag in the window does, so automation records it. The card names Age, Drive, Bias, Tape Speed and Output as the main controls.
- No control, preset, parameter ID, AU code or latency changed.

Tests: 666 pass, 0 fail (0.4.4 had 664). New: the card lists all 26 parameters by their IDs, and a text command sets Drive and lands in the card's done list. Of the existing lines only the CPU reading moved.

## 0.4.4, 2026-11-13

Fixes from the 2026-11-13 bug hunt (21 kept, all real, all fixed). Every fix has a check in `tests/PluginTest.cpp` (run alone with `PluginTest phase2`) that fails on 0.4.3 and passes now.

Luke left this call to the lead on 2026-11-11. Three of the fixes change what the default and the presets play, so like the old response they travel with the session: a new instance and every preset get them, and a session saved before 0.4.4 keeps what it had. All 16 states read out of the installed 0.4.3 (`tests/baseline/v0.4.3/`) play the same samples, bit for bit.

The sound:

- Auto Level leaves the head bump, the dip and Age's low cut alone, as the README always said. It heard the tape after them, so it took the bump for a level gain and the low cut for a loss. Auto Level on against off, a low tone against 1 kHz: 70 Hz at 15 ips -2.40 dB in 0.4.3, 0.01 now; 110 Hz at 30 ips -1.42, 0.01; 30 Hz at 3.75 ips -2.84, 0.07; 200 Hz (the dip) +0.31, 0.00; 30 Hz at Age 100 +5.32, +0.09. The old response keeps 0.3.1's.
- Dropout Rate is the number of dropouts that begin each second. A new one cannot begin while one is on, and the chance did not count that: 10 gave 6.6 a second, 5 gave 4.0, 2 gave 1.73. Now 9.95, 5.01, 1.88. Worn Cassette (0.7) and Old Radio Voice (0.5) get 3 and 2 percent more, and their dropouts fall at other moments.
- Head Azimuth follows Mix like the rest of the read head. At Mix 0 with Head Azimuth 100 at 3.75 ips each side was 0.2 ms off the input (null +2.8 dB); now -140.7 dB. No preset uses it.

Also fixed in the sound, for any session:

- A pre-0.4.0 session saved at Mix 100 with High Cut and the compander off takes 0.3.1's response when automation brings Mix, High Cut or the compander in. It got the new one for good (-24.2 dBFS apart after Mix went to 50); now the same samples as the old response.
- Age brought back to 0 while playing left its low cut's last value as an offset on everything after it (-65 dBFS on silence). It now lets go of it over a few tens of milliseconds (-165 dBFS).
- Texture turned back up in a quiet place used the level from when it was turned down: -36.7 dBFS of grain on silence. Its level follower now runs with Texture at 0 (-172 dBFS).
- With a dynamic compander, a block of 7, 100 or 1000 samples gave different samples than 512 (-54.7, -54.3, -62.8 dB). The shelves now move every 32 samples from the last reset (-148, -321, -321 dB). Blocks of a multiple of 32 give exactly what 0.4.3 gave.
- Host Reset: the Reset JUCE sends straight after the host turns its own bypass on is skipped, so the fade to dry plays out instead of cutting to an empty delay line (it differed by -12.1 dB from the plain switch; now identical). A Reset now takes the lock the host renders under. It already cleared everything prepare clears and allocated nothing.

The preset and the window:

- The preset last chosen is saved with the session, in the eight bytes after the settings (the saved XML stays as 0.3.1 wrote it). Reopened, it reads "Worn Cassette, edited", not "Tape Start, edited", and the arrows step from it. It is not applied again. A session saved before 0.4.4, or one whose number is damaged, names none: the box reads Choose a preset in the quieter colour, and the arrows go to the first or the last.
- A setting loaded with the window open opens or shuts Advanced as it was saved (the drawer stayed as it was while the arrow said the other, and the smallest height did not make room for it).
- Opening and shutting Advanced gives back the height it had. It took off the whole drawer even when the top of the window or the screen had cut the opening short (1200 high came back at 789 on this screen).
- Output has room for "+24.0 dB" (Tape speed gives it 26 of its 214 parts), and at the smallest window "Dictation Machine", "Dropout rate" and the curve's hint fit (the drawer's columns moved by up to 16 parts, and the hint is shorter).
- Top end and the picture follow a change of sample rate and of the response with no control moving (Top end read 24.0 kHz at 44.1 kHz).
- Hiss is the hiss leaving the plug-in: after the compander's decode, the cuts, the Device, Mix and Output. Against the real floor: Compander Fixed +7.0 dB off, now 0.0; Dynamic Strong +11.7, now -0.3; Mix 50 +5.9, now -0.1; Output +12 -12.1, now -0.1; High Cut 3 kHz +7.9, now -0.1. At Age 50 0.32, now 0.18.
- The Advanced line and the faint blocks are worked out from the parameters, so automation from the host's thread cannot leave them on the values before.
- Age shows every move: 29.7 then 30 kept saying Used reel, 30.2 then 30.6 kept 30%. Typing -20 sets 0, not 20.
- A window opened again shows nothing from the last time it was open (the saturation number read 2.2 dB and the spectrum -9 dB on silence).
- The pictures are drawn again only when what they show changes: an Output or Texture ride redrew both on every frame (60 rebuilds in 30 frames, now 0), and a dynamic compander's boost redrew the grid and the curve too (32 in 16 moves, now 0; the line still follows it).
- Double-clicking the Device block resets it: the list waited 200 ms, shorter than many double-clicks, and now waits for the double-click to finish (430 ms) and a second press stops it.

Tests: 91 new lines in `tests/results.txt`, all passing. Of the existing lines only the readings moved, all within their limits: Auto Level spread on the voice like signal 0.72 to 0.59 dB, dips at Dropout Rate 2 in 30 s 49 to 55 (set 60), the presets' level against dry by up to 0.23 dB (Worn Cassette -1.82 to -2.05), Hiss at Age 50 0.32 to 0.18 dB. No limit was touched.

## 0.4.3, 2026-11-11

Luke said yes to both on 2026-11-11.

- Every factory preset now uses the new response. Parallel Body, Airy Encode, Worn Cassette, Slow Reel Haze, Starved Grit and Old Radio Voice kept 0.3.1's response until 0.4.2. Measured before on the 11 working takes (0.4.2 then 0.4.3): Parallel Body gets brighter (16 kHz band -7.18 to -1.83 dB) because the blend of dry and tape no longer combs the top; Worn Cassette is half a dB quieter and its S 4.8 dB duller; Slow Reel Haze's S 1 dB duller; Starved Grit and Old Radio Voice move 0.1 to 0.15 dB.
- Airy Encode's Encode goes from 60 to 30. In Luke's chain it sits after Pro-DS, and at 60 it put 2.9 dB of S back. At 30 it puts back 1.10 to 2.12 dB, median 1.41 dB (S against the voice, 4 to 8 kHz, over the 11 working takes).
- A session saved with one of these presets keeps the sound it was saved with.
- Every real vocal check passes on all six presets on all 11 working takes. In the suite only the level lines of the six presets and the old-response preset line moved (573 pass, 0 fail).

## 0.4.2, 2026-11-11

Worn Cassette and Slow Reel Haze are safe in mono. Luke said yes to this on 2026-11-11. Both presets set Stereo Drift (30 and 25) and Head Azimuth (15 and 8), which pull left and right apart, so a mono voice lost level when the two sides were summed. Both controls are now 0 in these two presets. Their stereo movement is gone; everything else in them is as it was, and both still keep the old response.

- Measured on the 11 working takes, mono sum against the two sides, worst voiced quarter second (limit -6 dB): Worn Cassette -9.69 to -6.22 dB in 0.4.1, -2.82 to -0.15 dB now. Slow Reel Haze -6.00 to -0.70 dB in 0.4.1, -0.05 to 0.00 dB now.
- Over the whole take (limit -3 dB): Worn Cassette -2.24 to -1.62 dB in 0.4.1, -0.03 to -0.01 dB now. Slow Reel Haze -0.23 to -0.10 dB, now 0.00 dB.
- The real vocal mono check failed on 11 of 11 takes for Worn Cassette and 1 of 11 for Slow Reel Haze in 0.4.1. In 0.4.2 every real vocal check passes on every working take for both presets, with 0 clicks.
- The default and the other nine presets are untouched. The plug-in's own tests read the same as 0.4.1 apart from the CPU line (573 pass, 0 fail).
- A session that already uses either preset keeps the Stereo Drift and Head Azimuth it was saved with.

## 0.4.1, 2026-11-11

The tape stays on the reported latency when Tape Speed, Bias, Hardness or light Age change the top end. 0.4.0 lined it up for the default tone only: at 30 ips it came out 0.19 sample early (the real vocal test read 0.215 on Clean Reel) and at 7.5 ips 0.36 late. The short delay that lines the tape up now follows those filters, within 1.6 microseconds, for as long as there is delay left to take off: 30 and 7.5 ips, any Bias at 15 and 30 ips, Age to about 25. Past that (3.75 ips, high Age, Head Spacing) the tape comes out late by what is left: 0.6 sample at 3.75 ips, 2.2 at Head Spacing 100, as any low pass filter is late. Taking all of that off would cost about 15 more samples of latency, so it is left.

At Mix 100 this is timing only. In a Mix blend with the new response the dip on slow aged tape falls from 4.5 to 2.8 dB. The old response is untouched, so nothing made with 0.3.1 changes.

## 0.4.0, 2026-11-11

The tape now comes out exactly when the plug-in says it does. It reported 355 samples at 48 kHz and the tape path took 355.56: the real vocal test read 356 on every take. Between the oversampling filters sit the curve's averaging and two top end filters, flat in the band at the default tone but 11.7 microseconds late, and nothing had counted them. Logic lines tracks up by the reported number, so a parallel chain lost top end, and so did every Mix under 100 inside the plug-in. Now the tape is on the reported sample within 0.22 microsecond at 44.1, 48, 88.2 and 96 kHz. The reported latency is unchanged at 44.1 and 48 kHz (332 and 355 samples). At 88.2 kHz and up it is one sample more (596, 642), because there the old path was more than a whole sample late.

At Mix 100 the sound is the same, 11.7 microseconds earlier. With the tape lined up, Mix 50 no longer dips against the quieter path (0.06 dB, was 0.8) and the dip on slow aged tape is 4.5 dB (was 7.9; 2.8 dB from 0.4.1).

Also fixed:

- Dropouts could click. A dropout that began before the last one had recovered jumped to its new depth in one sample, by up to a quarter of the level. The depth now glides. One dropout on its own is unchanged.
- Head Azimuth at 0 was not quite 0, so left and right of a mono voice differed in the last bit and a stereo instance did not match a mono one. They are identical now.
- High Cut falls 12 dB per octave all the way to the top of the band. It fell far faster up there (27 dB in the octave above 10 kHz at 48 kHz).
- The compander's shelves follow the analog shape. They ran up to 0.5 dB over it at 10 kHz for 10 dB of boost and 1.3 dB for 20 dB.
- Auto Level leaves the tone alone, as the README always said it did. It compared the dry signal with the tape after the top end losses, so on sound that is mostly top end it turned the loss back up: with Age at 100, S sounds on the real voice came out 2.6 dB hotter against the voice than with Auto Level off, and a 10 kHz tone got all 14 dB back. Its detector now hears the dry signal through the same losses. At the default tone nothing changes.

Nothing made with 0.3.1 changes its sound. The lined up Mix, the new High Cut, the new shelves and the new Auto Level are the response of a new instance. Six presets keep the old response (Parallel Body, Airy Encode, Worn Cassette, Slow Reel Haze, Starved Grit, Old Radio Voice), and so does a session saved before 0.4.0 that uses any of those four things. It is not a control: it travels with the preset or session (README, "The old response"). All 26 parameters, their IDs, ranges and defaults are unchanged, and saved sessions load as before.

Tests: the limits that had been changed after a first run are back at their first values and pass (High Cut an octave out, the Mix dip on heavy tape, identical channels at Head Azimuth 0, the compander against the analog shape, dropouts counted from the latency on, every point of the window's tone line read). New tests hold the tape path to the reported latency in microseconds, a dropout to a dip without a step, Auto Level to leaving the top end losses alone, and the old response to the figures 0.3.1 gave for itself.

## 0.3.1, 2026-11-07

Renamed to Tape Color, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0

Its own window, called Tape Color, in deep brown, amber and cream. Age is the big idea: one wide ribbon that narrows from a fresh reel to a worn cassette, next to a flat reel that turns at the tape speed, shakes with the real read head and shrinks into a cassette hub. Above them is the real spectrum going in and coming out, the drawn tone of the tape, what Age has taken in rust, and the saturation curve with the part in use lit up. The main controls are in view and the rest open under Advanced in five sections. Every control is still the same AU parameter with the same ID, name and range.

The sound is unchanged: all 55 renders through the real AU, default, every preset and random automation at 44.1, 48 and 96 kHz and in mono, come out bit identical to 0.2.1. The window reads copies of the input and output that the audio thread writes only while a window is open. Only window size and whether Advanced is open are added to the saved state.

## 0.2.1

CPU work only. The sound is the same: renders of the default, every preset and random automation at 44.1, 48 and 96 kHz and in mono null against 0.2.0 at -114 dBFS or better, which is the last bit of the 32 bit output. Parameter IDs, defaults, presets, latency and tail are unchanged.

The oversampling filters now slide their history once per piece of audio and work out four outputs at a time. The oversampled path runs left and right side by side, with a faster form of its curve. The low level dead zone and Head Spacing are skipped while they are at 0. Device filters now work out four taps at a time. Values that only change when a control moves are no longer worked out again for every sample or piece.

## 0.2.0

Matched more of what a full tape color plug-in offers. New controls, all with new IDs: Tape Hardness, Head Spacing, Head Azimuth, Random Drift, Stereo Drift, Dropout Depth, Dropout Rate, Compander, Encode Amount, Decode Amount, Texture, Device and Device Mix. Three new presets. Oversampling doubled to about 768 kHz after loud high tones missed the aliasing limit, and the tape path grew to 6 ms to make room for the new read head movement. Readings for a later window are available from the processor.

## 0.1.0

First version. Oversampled tape saturation with Drive, Bias, Tape Speed, Wow, Flutter, Noise, Age, Low Cut, High Cut, Mix, Output, Auto Level and Bypass. Latency covers the oversampling and the tape path, and the dry path is matched to it.
