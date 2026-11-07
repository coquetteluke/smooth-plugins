# Changelog

## 1.2.0

A window of its own, in place of the plain list of controls. The sound is the same: 63 renders of the default, every preset and random automation at 44.1, 48 and 96 kHz and in mono are bit identical to 1.1.1, through the real AU. Parameter IDs, names, ranges, presets, latency and tail are unchanged. Sessions keep working.

The hero is a map of the comb filter: across is the wet to dry delay with zero in the middle, up is frequency. The notches are the curves that rush up and off the top as the delay nears zero. The real output spectrum is painted into it where the wet deck really is. A line moves with each side, with a tick at each notch it is making, and the zero line fires when a deck goes through. In Manual, dragging across the map rides Flange. In LFO it sets Depth.

The processor gains a lock free copy of its output for the window. The session now also saves the window size and whether Advanced is open.

## 1.1.1

CPU work only. The sound is the same: renders of the default, every preset and random automation at 44.1, 48 and 96 kHz and in mono null against 1.1.0 at -126 dBFS or better, which is the last bit of the 32 bit output. Parameter IDs, defaults, presets, latency and tail are unchanged.

The oversampling filters now slide their history once per block and work out four outputs at a time. Smoothed values that have reached their target are skipped.

## 1.1.0

Second pass against what a full tape flanger offers. New controls, all with new IDs: Mode, Flange, Fade, Invert Phase, Delay, LFO Shape, LFO Symmetry, LFO Phase, LFO Swing, Wow Depth, Wow Rate, Flutter Depth, Flutter Rate, Random Depth, Random Rate, Random Unsync, Input Gain, Output Gain, Low Cut, High Cut and Feedback Compensation. Six new presets. Readings for a later window are available from the processor. Default settings and the first seven presets are unchanged, held to a saved render of 1.0.0, and the latency is the same. Tail length now reports 4 seconds to cover Delay at its longest. Output is clamped to +/-16 as a last guard.

## 1.0.0

First version. Through-zero tape style flanger with Rate, Tempo Sync, Sync Division, Depth, Feedback, Saturation, Tone, Spread, Mix and Bypass, seven factory presets, and a fixed reported latency.
