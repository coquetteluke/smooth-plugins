# Changelog

## 1.3.0, 2026-11-17

Switchboard can now see and set every control through the link. Sound unchanged.

- The link is `src/SmoothLink.h`, a byte copy of Switchboard's. It puts a card of every parameter and its value in the shared link folder, and applies set commands by moving the real parameter with a begin and end gesture, as a drag in the window does, so automation records it. The card names Rate, Depth, Feedback, Mix, Saturation and Tone as the main controls.
- Tests: 454 pass, 0 fail (1.2.8 had 452). New: the card lists all 31 parameters by their IDs, and a text command sets Feedback and lands in the card's done list.
- Latency, controls, IDs and AU codes are unchanged, and sessions load as before.

## 1.2.8, 2026-11-13

A synced sweep is heard on the beat. Luke said yes to this on 2026-11-13: "u can do all of these".

- With Tempo Sync on and the host playing, the sweep follows the song position less the reported latency. The host's playhead gives the position of the audio coming in, and the host plays the output one latency later to line it up, so up to 1.2.7 the sweep was heard 324 samples (6.75 ms at 48 kHz) early. Read off the output against the timeline it was 6.25 ms early at 1/16, 1/8 and 1 bar of 3/4, at 44.1, 48 and 96 kHz; now 0.25 to 0.5 ms, inside the reading's own step. The 1 bar lock check went from 0.028 to 0.006 ms rms, the 3/4 one from 0.037 to 0.007.
- What changes: only the synced presets, and only with a host playing. One Bar Swirl, Quarter Note Wobble and Square Flip sit 6.75 ms later against the beat at 48 kHz (1.2, 4.9 and 2.4 degrees of their sweep at 120 BPM). Through the real AU with a host playing, the default and the 10 other presets are bit identical to 1.2.7, and the three synced ones match 1.2.7 given a song position one latency on to -150 dBFS. With no host (no transport, as the real vocal checks run) every file is bit identical. `tests/null_results.txt`.
- Test changed with Luke's yes: "same output with a playhead at 120 BPM or none, synced" compared a playhead at ppq 0 with no playhead, which is the 6.75 ms error the fix takes out. It now reads "..., synced, the playhead one latency on": the output with no playhead must still match a playhead one latency on to -80 dB, the same limit. The old text and numbers are in a comment above it. The free line is unchanged.
- The 1.2.7 INFO line (0.919 ms rms at 1/16) is now 4 checks: the sweep heard within 1 ms of the timeline. All four failed on 1.2.7 (5.75 to 6.35 ms early). Most of the old 0.919 ms was the reading's scatter with 2 ms frames, so the checks read the timing itself.
- Six other lines moved a little and still pass: the two lock checks above, and four synced rate readings (3.113 to 3.111 Hz, 2.998 to 2.999, 1.999 to 2.000), which read a sweep that now starts 6.75 ms further on.
- NullRender can play a host transport (`transport`, and `transport+latency` for the proof above). Latency, controls, IDs and AU codes are unchanged, and sessions load as before.

## 1.2.7, 2026-11-13

Phase 2 bug fixes. The default and every preset play the same samples as 1.2.6, and sessions load as before. No control, preset, ID or AU code changed, and latency is the same. No sound call was needed.

- Tempo Sync in a cycle region: when the region was one sweep long (One Bar Swirl on a 1 bar loop, 8 bars on an 8 bar loop) or any odd number of sweeps, the sweep raced through a whole extra cycle in about 150 ms at every loop point, a pitch swoop on the wet. With Swing at 0 a jump of whole sweeps now leaves it where it is. With Swing the long and short cycles still have to be put back in order, so that case still moves.
- Tempo Sync follows the time signature: the bar values (8, 4, 2 and 1 bar) are bars of the song's meter, so 1 bar in 3/4 is 3 quarter notes, and the lock keeps to the host's bar lines. 1.2.6 took every bar as 4/4. With no time signature from the host, or in 4/4, nothing changes.
- LFO Phase glides the short way round: from 359 to 1 degree it moves 2 degrees. 1.2.6 swept back through the whole cycle in about a quarter second.
- The host's Reset (a stop or a locate) now does what a prepare does: empty delay lines, no old feedback tail, the sweep and drift back at their start, and the next start snaps to the bar. 1.2.6 ignored it, so a Feedback 90 tail could ring into the next start. The Reset JUCE sends straight after the host's own bypass switches on is skipped, so Bypass still fades.
- The preset box names the preset last chosen, and the session keeps it. A new instance, and a session saved before 1.2.7, read "Choose a preset" (1.2.6 named Slow Zero Crossing, which they were not). The arrows go from there to the first or last preset.
- Typing a Length matches the names in any case ("1 Bar"), and text that is no Length is ignored. 1.2.6 took any slip for 8 bars.
- Window: Length is no longer cut to "Len" beside the Sync pill with Advanced open (the first cell gets room for its name). Holding Shift to fine tune Depth on the early side of the field no longer turns the drag round. Quiet sound now lights the comb picture instead of darkening it (the violet was premultiplied twice). The Bypass hint said "Fades the wet out", which has not been true since 1.2.2; it now says the input passes untouched. The Mix hint says Mix 0 is not a bypass.
- Checked and not changed: a state loaded onto a plug-in in use (a Logic user preset, pasted settings) already gives every control it does not name its default; the hunt said it kept the old values, and a test now holds that it does not. Not fixed: a synced sweep is heard one reported latency early after the host's latency compensation (6.75 ms at 48 kHz); fixing it would break "same output with a playhead or none", so it is in the README for Luke.
- Tests: 41 new checks and 1 INFO line in `tests/results.txt` (448 pass), 14 new in `tests/window_results.txt` (60 pass). The 407 old lines and the 46 old window lines print the same numbers as 1.2.6.

## 1.2.6, 2026-11-12

Quarter Note Wobble no longer plays quieter than the rest. Luke left this call to the lead on 2026-11-11, and the lead chose it.

- Loading Quarter Note Wobble now sets Output Gain to +1.1 dB (it was 0). On Luke's real takes it dropped the voiced level by up to 3.63 dB (limit 3 dB; 4 of the 11 working takes past it: 09 -3.63, 10 -3.51, 14 -3.36, 16 -3.08). +1.1 dB is the smallest 0.1 dB step that brings every take inside 3 dB with the margin 1.2.3 left its three presets (about 0.5 dB).
- Nothing else in the preset changed. The 1.0.0 render test now expects this preset's 1.0.0 render raised by exactly 1.1 dB, a deliberate change named in the test; it nulls at -142.1 dB. The default and the other six of the first seven presets are held to the 1.0.0 render as before.
- In the test suite only two lines moved: this preset's level on the test signal, -2.961 to -1.861 dB (+1.100), and its line against 1.0.0, now read against the raised render. The other 405 lines are identical to 1.2.5.
- Sessions that already use Quarter Note Wobble keep the Output Gain they were saved with.

## 1.2.5, 2026-11-12

A control moved while playing now lands exactly where it is set. Found on the second real vocal pass.

- The glide behind Mix, the gains, Feedback, Invert Phase, Depth, Spread, Delay, Tone and the other smoothed controls works in 32 bit floats. Near its target a step became smaller than the last bit of the number and the value stopped short for good, about 0.01 % under a target near 1. Now, when a step no longer moves it, it lands on the target. Tone's shelf is worked out once more when it lands.
- What it fixes, measured in the new tests: Invert Phase switched on while playing (Depth 0, Mix 50) left the dry 87.5 dB down where the README says it cancels by more than 160 dB; it now leaves it 167.6 dB down. Mix moved to 100, Output Gain, Input Gain, Feedback and Depth moved while playing landed 59 to 81 dB off the setting, Spread 51 dB; now -207 dB or identical samples. A factory preset picked while playing (as Logic does) sat 38 to 60 dB off the same preset set before the plug-in started, on 12 of 13 presets (Off Zero Double held Delay 0.0003 ms short); now -157 dB or better.
- Luke left this call to the lead on 2026-11-11. The sound with nothing moving is the same: the default and the first seven presets still match 1.0.0 at -110 dB, and through the real AU the default and Manual Ride are bit identical to 1.2.4. A preset picked on a running plug-in, and automation, now reach the values they ask for; against 1.2.4 that differs by at most -45 dBFS peak (Off Zero Double at 96 kHz), `tests/null_results.txt`.
- Saved sessions load as before. Latency, parameters and AU codes are unchanged.
- Tests: 21 new lines (8 for controls moved while playing, 13 for presets picked while playing), 19 of them failed on 1.2.4. The Feedback step limit at Mix 50 is back to the 1.5 dB it was first written with on 10 of its 12 steps; the first step with the wet ahead of the dry stays at 0.3 dB (measured 0.58 and 0.45 dB), the reason is in the test. The other 386 lines print the same numbers as 1.2.4.

## 1.2.4, 2026-11-11

Phase Flip Thin stays whole in mono. Luke said yes to this on 2026-11-11.

- Loading Phase Flip Thin now sets Spread to 0 (it was 20). With Invert Phase and Spread 20 the two sides cancelled at different moments around a crossing, so on Luke's real takes the mono sum fell up to 11.9 dB under the two sides for a quarter second (take 10 at 34.65 s; 7.2 to 11.9 dB on ten of the eleven working takes, limit 6 dB).
- The cost: the preset has no stereo width of its own any more. Both sides are the same.
- Nothing else changed. Every other preset and the default are the same samples as 1.2.3.
- Sessions that already use Phase Flip Thin keep the Spread they were saved with.

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
