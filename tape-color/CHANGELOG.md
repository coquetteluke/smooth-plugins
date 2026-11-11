# Changelog

## 0.4.0, 2026-11-11

The tape now comes out exactly when the plug-in says it does. It reported 355 samples at 48 kHz and the tape path took 355.56: the real vocal test read 356 on every take. Between the oversampling filters sit the curve's averaging and two top end filters, flat in the band at the default tone but 11.7 microseconds late, and nothing had counted them. Logic lines tracks up by the reported number, so a parallel chain lost top end, and so did every Mix under 100 inside the plug-in. Now the tape is on the reported sample within 0.22 microsecond at 44.1, 48, 88.2 and 96 kHz. The reported latency is unchanged at 44.1 and 48 kHz (332 and 355 samples). At 88.2 kHz and up it is one sample more (596, 642), because there the old path was more than a whole sample late.

At Mix 100 the sound is the same, 11.7 microseconds earlier. With the tape lined up, Mix 50 no longer dips against the quieter path (0.06 dB, was 0.8) and the dip on slow aged tape is 4.5 dB (was 7.9).

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
