# Changelog

## 1.2.3, 2026-11-11

Three presets no longer play quieter than the rest. Luke said yes to this on 2026-11-11. On his real takes they dropped the voiced level by more than the others: Square Flip by 5.6 dB, Phase Flip Thin by 5.4 dB, Triangle Jet by 4.0 dB (the default drops it by 2.7 dB).

- Loading Triangle Jet now sets Output Gain to +1.5 dB, Square Flip to +3 dB, Phase Flip Thin to +3 dB. Every other preset still sets Output Gain to 0 dB.
- Nothing else in those presets changed. Phase Flip Thin keeps its Spread of 20: its dip in mono near a crossing is a separate question for Luke.
- In the test suite only three numbers moved: the level of those three presets on the test signal, by exactly +1.5, +3 and +3 dB (Triangle Jet -5.00 to -3.50 dB, Square Flip -3.28 to -0.28 dB, Phase Flip Thin -3.15 to -0.15 dB against the input). The other 383 results are identical to 1.2.2, and the default and the first seven presets still null against 1.0.0.
- Sessions that already use one of the three presets keep the Output Gain they were saved with.

## 1.2.2, 2026-11-10

Bypass is now a true bypass. Found on Luke's real vocal takes: with Bypass on, the output did not null against the input, it sat 66 to 74 dB under it for the whole take. Bypass used to be Mix 0, and the dry path runs through the oversampling filters, which have a ripple of a few thousandths of a dB and roll off above 21 kHz. Now the bypassed output is the input itself, delayed by the reported latency (324 samples at 48 kHz, unchanged), bit for bit.

- Switching Bypass fades between the effect and the input over 20 ms on a raised cosine. The old fade left a tick 76 dB under a steady tone above 8 kHz, the new one is at -125 dB.
- The effect keeps running under Bypass, so switching back picks up exactly where it would have been. Before, the wet came back 0.01 % low and stayed there.
- The sound with Bypass off is the same samples as 1.2.1: the default and every preset at four formats are bit identical through the real AU, and so are renders with every control but Bypass moving at random.
- One test changed what it compares with: "Bypass against Mix 0" could only pass while Bypass was Mix 0, and it is why the tests never saw this. It now holds Bypass to the delayed input at -120 dB, and the old reading is printed next to it.

## 1.2.1, 2026-11-07

Renamed to Tape Flanger, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

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
