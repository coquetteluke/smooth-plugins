# Changelog

## 0.6.1, 2026-11-12

0.6.0's default change is undone: Steady Notes is off in a new instance again, as in 0.5.1. With it on, Sustain did nothing to drums or anything else without a pitch, so every new instance lost Sustain there, and the change fixed none of the real vocal check's failing lines, because the check turns Steady Notes off itself for those. A new instance also showed "Neutral, edited". The lead's call, 2026-11-12.

- Kept from 0.6.0: a session saved before Steady Notes existed loads with it off; a session saved with it on (0.6.0 or earlier) loads with it on, and one saved with it off loads off. The window draws Sustain's tail with Steady Notes on.
- Fixed: with Steady Notes on, Sustain did nothing to a sound with no pitch. Steady Notes is there to keep held notes still, not to switch Sustain off where there is no voice. Now, where there is no voice, Sustain is what it is with Steady Notes off: on a track with no pitch at all, such as drums, from the start, and after a voice once no note has been heard for 1 s, coming in fully by 1.5 s. Breaths and mouth sounds between phrases come well within that and are still left alone. On a train of noise bursts at Sustain +9 and -9 every tail now moves exactly as with Steady Notes off (0.000 dB apart; 0.6.0 left them 9.0 dB apart). Before the first note of a vocal take there has been no note either, so breaths and mouth sounds there get Sustain as with Steady Notes off: on the working set at Sustain +6, breaths at most 1.3 dB and mouth sounds at most 0.8 dB against the voice (limit 3).
- With every other control at its default, the output is the same as 0.5.1's and 0.6.0's sample for sample.

Tests: 458 pass, 0 fail (0.6.0 had 450). New: noise bursts with no pitch, from the start and 2 s after a sung note, at Sustain +9 and -9. The checks that 0.6.0 turned to "on by default" are back to off.

## 0.6.0, 2026-11-12

Steady Notes is on in a new instance. Luke left this call to the lead on 2026-11-11.

- What a new instance sounds like does not change: with Punch, Attack and Sustain at 0, Steady Notes changes nothing. Measured through the real plug-in, the default of 0.5.1 against the default of 0.6.0 on all 11 working set takes: identical, 0 samples differ. Once Punch, Attack or Sustain is moved, a new instance now gets the voice behaviour that the Steady Notes groups of the real vocal check measure (every one of those limits met on all 11 takes).
- Saved sessions keep their sound. A session saved since 0.4.0 holds a value for Steady Notes and keeps it, on or off. A session saved before 0.4.0 has none and loads with it off. The 14 older presets still turn it off; the six lead presets still turn it on.
- A new instance shows "Neutral, edited" in the preset box, because Neutral has Steady Notes off.
- For drums or anything without a pitch, turn Steady Notes off: with it on, Sustain does nothing to a sound with no pitch.
- The window's preview drew no Sustain at all with Steady Notes on (a drawn hit falls more than 6 dB before it could count as a note). It now draws the hit's body as the note and the tail after it the way a voice gets it.
- The pitch sense now runs at the default: the worst audio block on the working set took 1.3 to 2.1 % of one core, against at most 1.0 % in 0.5.1.

The 14 working set lines that fail are the groups the real vocal check runs with Steady Notes set off on purpose. They read exactly as in 0.5.1, because the check sets Steady Notes off itself and does not use the default.

Tests: 450 pass, 0 fail (0.5.1 had 439). New: a new instance has Steady Notes on; a session from before 0.4.0 loads with it off into a new instance and into one that had it on; 0.5.1 sessions saved with it off and on keep it; a damaged value goes back to the default; the default's output with it on and off is the same sample for sample on two real voices and the sung line; the preview draws Sustain with it on. Two older checks follow the new default: the control list now expects Steady Notes to default to on, and the window check "Sustain moves its tail" turns Steady Notes off first, so it checks the same drawing as before.

## 0.5.1, 2026-11-11

Two fixes to Steady Notes and a ceiling for every lead preset, from the third real vocal pass. With Steady Notes off nothing changes: every older preset and every saved session that does not use it sounds exactly as before.

- Steady Notes, Sustain: a held note that starts loud and settles lower is now left still. Inside a note the sag is also measured from the highest the note stood in the last 100 ms, not only from the slow envelope, which after a loud start was still coming down, so a small dip of the settled note read as a note dying away. This was the one Steady Notes miss of the blind set (take 15, Sustain +9, held vowels +0.25 dB on average, limit 0.2). The same thing on working take 04 at 149.86 s (7.6 dB for 30 ms) and take 02 at 223.13 s; on the working set the held vowel average went from at most 0.01 to 0.00 dB. On a made-up note that does this, 5.8 dB to at most 0.02 dB at Sustain +9 and -9.
- Steady Notes, Punch: the level match now counts as right within 0.05 dB before it is held, instead of 0.5 dB. A steady tone through Lead Gentle came out 0.19 dB low (now 0.02); on the first session's take 01, Punch 50 settled 1.75 dB low (limit 1.0, now 0.20).
- Lead Gentle, Lead Soft Consonants, Lead Long Tails and Lead Dry And Close now have Ceiling On at -1 dBFS, like Lead Upfront and Lead Rap. On the first session's hot takes 06, 08 and 10, Lead Gentle and Lead Dry And Close went up to 0.8 and 1.0 dB over 0 dBFS; now they peak at -1.5 to -1.8 dBFS. On Luke's Rode takes they peak 6 dB or more under the ceiling, so nothing there changes. A session saved with one of these presets keeps the values it was saved with. Luke left this call to the lead on 2026-11-11.

On the 11 working set takes every Steady Notes limit and every limit of the six lead presets is met, and every group with Steady Notes off gives 0.5.0's number exactly. Latency, AU codes and parameters are unchanged.

Tests: 439 pass, 0 fail (0.5.0 had 410). New: a held note that starts loud and settles 10 dB lower, at three rates. The presets test that 0.5.0 had narrowed to the seven presets of 0.2.0 covers every preset again (Ceiling On, under the ceiling on hot material, a -30 dBFS tone not taken down), and the Steady Notes preset test looks at every preset again.

## 0.5.0, 2026-11-11

Steady Notes now follows the note, Punch keeps a held note still, and there are six presets for a lead vocal. Off, nothing changes.

Why: 0.4.0's Steady Notes kept held vowels still but could not meet all of its own limits on Luke's real takes. Breaths, mouth sounds and the gain after a phrase ends pulled against each other, because it read level only, and by level alone the end of a word, the breath after it and a sag inside a held note look the same.

What Steady Notes does now (all of it only while the switch is on):
- It hears pitch, the way a pitch tracker does. A note is a sound that has kept a pitch for 80 ms. While it has its pitch it may sag 6 dB before Sustain acts.
- The tail is what comes right after the pitch stops. A boost stays on it for about 70 ms and is gone by 105 ms. A cut stays for 110 ms and is gone by 130 ms. The breath, the mouth sound and the next phrase after that are left alone. Only a boost spares an S.
- Punch reads a held note by its level instead of every peak, and its level match is learned on the voice and then held, so a held note keeps one gain. The room and far-off breaths are no longer lifted with the voice.
- Attack no longer takes a jump of the peaks inside a held note for a start.

On the 11 working set takes with Steady Notes on, every limit is met: after a phrase ends +5.6 to +7.8 and -6.3 to -9.0 dB of 9, held vowels 0.00 dB, breaths at most 1.3 dB against the voice at Sustain +6 (limit 3), mouth sounds and S not raised, Punch 50 and 100 add at most 0.56 and 0.64 dB of swing to held vowels (limit 1.5; 1.68 and 4.10 without).

New presets, added at the end of the list so every older preset keeps its number and its sound: Lead Gentle, Lead Upfront, Lead Rap, Lead Soft Consonants, Lead Long Tails, Lead Dry And Close. All have Steady Notes on, and each meets every limit on every working set take, sung and rap. The README says which of the 14 older presets suit a voice.

Off stays off: with Steady Notes off the detector is 0.4.0's sample for sample (a new test runs both side by side at all 14 older presets), and the real plug-in nulls against the installed 0.4.0 on every working set take at the default and three older presets. The default is still Neutral. Latency, AU codes and every parameter are unchanged; no parameter was added.

The window: the preset box is wider, so the longest name fits with ", edited" after it. The Steady switch's hint says what it does now.

Tests: 410 pass, 0 fail (0.4.0 had 347). New ones use a made-up sung line with a held note, what is left after it, a breath, a mouth sound, a quieter second note, two syllables on one breath, a breath straight after a note and a humming room.

## 0.4.0, 2026-11-11

Steady Notes: a new control, off by default, that makes Sustain fit a voice. Found on Luke's real takes (Rode session, 17 songs): Sustain moved held vowels in and out by its full amount, because every small sag of a held note read as a tail; it gave a quiet phrase start the tail gain of the louder phrase before it; and it lifted every S. With Steady Notes on, a sound that has kept its level for 60 ms may sag by 4 dB before Sustain acts, a sharp start counts as a new sound, and bright sounds (an S) are left alone. The tail of a note still gets the full amount.
Off, nothing changes: every preset and saved session sounds exactly as before, held to the numbers recorded from 0.1.0. No preset turns it on yet: which presets should is Luke's call after listening. The control is a real AU parameter (Steady Notes) with a switch at the end of Sustain's bar in the window. Attack, Punch, Impact, the Ceiling, the default and latency are unchanged.
New tests: a held note swinging 3 dB at 5 Hz moves by 7.3 dB under Sustain 9 without Steady Notes and by 0.0 dB with it; a quieter note after a louder one; a hiss after a note; the control's default, save and recall, and that every preset leaves it off.

## 0.3.1, 2026-11-07

Renamed to Punch, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0, 2026-11-07

The custom window replaces the generic one. Black and hot orange. The last hit drawn large, before and after, on a time scale that opens up its first moments, with the last few seconds as bars above it. Attack sits under the start of the hit and Sustain under its tail. In and Out meters share the plot's level scale, and Impact and Ceiling reductions hang beside them with large numbers. The Ceiling can also be dragged as a line on the plot. Every control is the same AU parameter under the same ID. The only new saved items are the window size and whether Advanced is open, kept in range when loaded.
No DSP change. Adds a lock-free read of the level before and after for the display. Output is bit identical to 0.2.0 through the installed AU, on real speech under four settings. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, `tests/null/`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`.

## 0.2.0

Second pass, checked against what the plug-in this replaces offers. Adds Gain, Impact (level-riding compression that also raises the level), Ceiling and Ceiling On, plus read-only Input Level, Output Level, Impact Reduction and Ceiling Reduction. Seven new presets. Latency stays 2 ms. Every first version control, preset and saved session sounds the same, measured against numbers recorded from 0.1.0.

## 0.1.0

First version. Transient shaper and fast compressor with Punch, Attack, Sustain, Mix and Bypass. 2 ms latency, dry path delay-matched. Seven presets. Damaged saved state and bad input samples are cleaned before they reach the audio.
