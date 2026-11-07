# Changelog

## 0.2.1

CPU work only. The sound is the same: renders of the default, every preset and random automation at 44.1, 48 and 96 kHz and in mono null against 0.2.0 at -114 dBFS or better, which is the last bit of the 32 bit output. Parameter IDs, defaults, presets, latency and tail are unchanged.

The oversampling filters now slide their history once per piece of audio and work out four outputs at a time. The oversampled path runs left and right side by side, with a faster form of its curve. The low level dead zone and Head Spacing are skipped while they are at 0. Device filters now work out four taps at a time. Values that only change when a control moves are no longer worked out again for every sample or piece.

## 0.2.0

Matched more of what a full tape color plug-in offers. New controls, all with new IDs: Tape Hardness, Head Spacing, Head Azimuth, Random Drift, Stereo Drift, Dropout Depth, Dropout Rate, Compander, Encode Amount, Decode Amount, Texture, Device and Device Mix. Three new presets. Oversampling doubled to about 768 kHz after loud high tones missed the aliasing limit, and the tape path grew to 6 ms to make room for the new read head movement. Readings for a later window are available from the processor.

## 0.1.0

First version. Oversampled tape saturation with Drive, Bias, Tape Speed, Wow, Flutter, Noise, Age, Low Cut, High Cut, Mix, Output, Auto Level and Bypass. Latency covers the oversampling and the tape path, and the dry path is matched to it.
