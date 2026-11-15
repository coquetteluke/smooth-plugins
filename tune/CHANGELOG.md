# Changelog

## 1.2.3, 2026-11-14

Luke asked for this on 2026-11-14: "do those 4 then".

- **In Low Male and Wide Range, a vowel cut off by an S is corrected to its end.** The tracker calls the sound unvoiced as soon as the S reaches the newest input, while the grains are still 20.6 ms behind with marks in the vowel they have not read, so the vowel's last 7.1 ms (343 samples) came out as sung. Now the grains read those marks with the correction the vowel had, then go back to the input. Test `PluginTest tail` (in `phase2`), written and run on 1.2.2 first: played as sung 7.146 ms (FAIL) to 0.000 ms (limit 2), the last 7.1 ms nearer the corrected vowel going on by -308.9 dB (FAIL) to +19.1 dB (limit above 0), the S as sung from 2.8 ms after the vowel's end (limit 5). Alto/Tenor (0.5 ms) is left as it was.
- **Only where a vowel at its own level is cut off by a loud, bright consonant.** Round 3 carried the correction on at every loss of voicing; each of its three versions made 8 to 18 real vocal check lines fail that pass on 1.2.1 (fades, dips inside a word, a note's end the tracker lost for a moment). Now the carry needs a vowel of 100 ms or more, the newest 4 ms crossing zero 14000 times a second or more (every spot round 3 broke read 240 to 10000; an S 15000 to 24000) and no more than 12 dB under the note, and the 10 ms before it no more than 6 dB under the note. On Luke's 11 working takes in Low Male that is 5 vowel ends in 32 minutes.

The default, every preset, Alto/Tenor and Soprano play bit for bit as 1.2.2: every other suite line reads the same number, the installed 1.2.1's sessions for the default and all 8 presets still play bit for bit (`tests/baseline`, no pin written again), and the default render of takes 14 and 09 has the same sha256 as the installed 1.2.2's. Real vocal rerun, working set, default with every extra render and the 8 presets: 0 lines passed then failed against 1.2.1 and 0 against 1.2.2; no sound measurement in the default run moved (only the CPU timing ratio of the denormals check). Parameters, IDs, ranges, defaults, presets, latency and AU codes are unchanged.

## 1.2.2, 2026-11-13

The Phase 2 bug hunt's faults, each with a test in `tests/Phase2Tests.h` (`PluginTest phase2`) written and run on 1.2.1 first (31 lines failed there). The default and every preset play exactly as in 1.2.1: every old suite line reads the same number, and the installed 1.2.1's own sessions for the default and all 8 presets play bit for bit (`tests/baseline`). Parameters, IDs, ranges, defaults, presets, latency and AU codes are unchanged. No sound call was made.

- **A host Reset clears the delay line.** It did nothing, so after a locate, a stop and start or the start of a bounce the first 13.7 ms came out as the last audio played, and the next note started from the old one's target. Now it clears what preparing does and allocates nothing: a vowel, a Reset and silence gave -10.8 dB of old vowel, now nothing; play, Reset, play equals a new instance (46783 samples differed). The Reset JUCE sends right after the host's own bypass turns on is left out, so Bypass still fades.
- **The window places the output right when the voice is far from its note.** Pitch Off Target, a meter, stops at +/-100 cents, and the window added it to the correction. With only C on Tune and E3 sung it put the output 300 cents off and said "100 cents sharp", though the output was on C. Now 0.0 cents off, the note turns lime, and it says 400 cents sharp. The host's readings are as they were.
- **Transpose +/-24 keeps the correction on top.** The shifter stopped at exactly 24 semitones, so at +24 a note sung 30 cents flat came out 30 cents flat (-24: sharp). Now 0.0 cents. Nothing under 24 semitones changes.
- **The chosen preset is saved** and named when the session is opened again (it always read "Natural Pop, edited"). A session saved before 1.2.2 reads "Choose a preset" and the arrows go to the first or last preset.
- **Custom notes on Bypass are left as sung** whatever Natural Vibrato is: with Robot Flat's -100 a bypassed note kept 26.5% of its vibrato, now 99.1% as measured, and the output is the input at the reported delay (+3.6 dB off before, -330.3 dB now).
- **Under Bypass, Correction Applied reads 0** (it read the correction it was not applying, -30 cents).
- **Readings read back as the value they show** (HostStress paramtext: "no note" read back as C-1, "B Custom" as C Chromatic).
- **Follow Key in an offline bounce or freeze** reads the key file every 0.3 s of audio, not of wall time, so a key change lands where it does in playback: 0.245 s of song after it (it was not taken up in the 5 s rendered). A Manual key from Key 1.7.2, written with `listening` false, is followed as before.
- **The window:** a state loaded with the window open (a Logic setting, Compare, undo) brings Advanced's drawer and height with it; with Follow Key on it shows and builds Custom notes from the key it really uses, also while the host is stopped (it showed and used the last processed key); the closed Advanced row names what fits and counts the rest ("Formant off" is shorter); the Sung and Correction readouts fit beside the note in tall, narrow windows ("-600 cents" was 38.9 pt too wide at 840 x 1200). Window CPU 2.60, 2.63, 2.82% of a core.

Not changed: Transpose typed with a plus sign already worked (JUCE reads integers with `atoi`; kept as a test). Not fixed, Luke's call: the last 7.1 ms of a corrected vowel going straight into an S in Low Male and Wide Range (0.5 ms in Alto/Tenor) are played as sung, because the tracker has no reading for a vowel's last periods; correcting them means carrying the pitch into the consonant, which changes the default at every word end. It is measured as INFO in `PluginTest phase2`.

## 1.2.1, 2026-11-12

**Robot Flat is flat.** Natural Vibrato worked on the voice as sung, so where the correction had already taken the vibrato out, -100 put it back upside down: Robot Flat's settings (Retune 0, Humanize 0, Flex 0, Natural Vibrato -100) kept 93.8% of a +/-40 cent vibrato, inverted. It now works on the vibrato left after correction, as the README says, and a new note starts its average where it comes out. Luke left this call to the lead on 2026-11-11 (it changes how Robot Flat sounds). Test `robot`, written and committed before the change: vibrato kept 93.8% (FAIL) to 1.03% (limit 10, Hard Tune's); a slow rise of 2 semitones never falls by more than 0.10 cents in 10 ms (limit 10).

With Natural Vibrato at 0, the default and every other preset, nothing changes: the suite is 1.2.0's to the last digit except Natural Vibrato -100 at Retune 400 ms, which now leaves 26.4% of the vibrato (19.6%, limit 45), and +100 191.1% (192.2%, limit 160 or more).

Luke's 11 working takes, Robot Flat on the song key (realvox): held notes 90th percentile 15.1 to 3.2 cents (1.1.1: 17.7), left as sung 2 to 0, slides turned round 10.59 to 2.90% on the sung takes and 7.12 to 3.87% on the rap takes (limit 5), wrong note jumps 69 to 11. It now reads exactly as Hard Tune does. Parameters, presets, latency and AU codes are unchanged.

## 1.2.0, 2026-11-12

The engine fixes Tune Pro found on Luke's own takes (17 whole songs, Rode NT1, 2026-11-10), ported into Tune's own copy of the engine they share, each with Tune Pro's test run on Tune first (tests committed before any fix: `PluginTest rode`). Luke left this call to the lead on 2026-11-11: the default and every preset sound a little different where a note is low, on the line between two notes, or just starting. Parameters, IDs, ranges, defaults, presets, latency and AU codes are unchanged, so saved sessions load as they were.

- **Alto/Tenor follows notes under its range, down to 70 Hz, at the same latency** (Tune Pro 1.6.0). Luke sings down to 77 to 95 Hz; those notes came out exactly as sung. A second reading speaks only where the range's own reading says "not voiced"; notes at 99 to 440 Hz are read bit for bit as before.
- **The pitch control is read in line with the audio** (Tune Pro 1.6.0 and 1.6.1). The correction ran up to 13.5 ms ahead of the sound it belonged to (Low Male and Wide Range at 262 Hz); notes older than the whole delay ran up to 4.6 ms late. The age is 1.80 periods, as Tune Pro's grains on the marks have it, measured on Tune's own test voice.
- **A voice that is not being corrected is the input at the reported delay**, also after a correction (Tune Pro 1.6.0). With every Custom note on Bypass the output was 6 to 16 dB under the input on Luke's takes (limit -80); now -332.7 dB on all 11 working takes in all four Input Types.
- **A held note keeps its target through a waver across the line, and a slow slide lands** (Tune Pro 1.6.0 and 1.7.1). The slide time is 0.10 s where Tune Pro has 0.12 s, since Tune's default Retune 20 ms adds about 35 ms of glide (0.12 read 156 ms on the slide test, limit 150).
- **Nothing before a sound starts.** Grains do not read ahead of where they are heard for the first 10 ms after they come in from a pass. Tune Pro's onset gate (1.7.2) was tried first: on Tune's plain grains its crossing to the input made clicks on takes 02, 05 and 07, as it did on Tune Pro's Classic, so it is not used.

| Suite (synthetic voices) | 1.1.1 | 1.2.0 | Limit |
|---|---|---|---|
| Alto/Tenor, held notes at 78 to 92 Hz, output pitch error at instant speed, worst 95th pct | 35.0 cents | 0.004 | 5 |
| The same at 87 Hz with vibrato and breath / a phrase A2 E2 A2 F#2 | 27.7 / 30.0 cents | 1.33 / 1.52 | 5 |
| Pitch movement against the sound, 147 to 262 Hz, worst | 13.5 ms early | 0.22 ms | 1 |
| The same on low notes (Alto/Tenor 101 to 138, Low Male 71 to 95 Hz) | 4.6 ms late | 0.55 ms | 1 |
| A voice left uncorrected after a correction / never corrected, against the input | +5.6 / -4.1 dB | -200 / -200 dB | -90 |
| A note on the line with +/-20 cents vibrato: waver / distance from the nearer note | 45.4 / 34.3 cents | 19.4 / 13.9 | 20 / 15 |
| A step landing 25 cents flat, at once / as a 60 ms glide | 6 / 2 ms | 14 / 8 ms | 20 / 14 |
| A held note sliding onto the next at 13 cents per 100 ms | 122 ms | 136 ms | 150 |
| Nothing before a note starts, the 10 ms before against the held note | -21.1 dB | -137.5 dB | -40 |
| Entries that hold their pitch, Retune 0 / default, median and 90th pct (new) | 20, 30 / 35, 45 ms | 20, 30 / 40, 45 ms | 20, 40 / 60, 100 |

Other suite numbers that moved, all inside their limits: Retune Speed 25 to 400 ms now measures 28, 53, 103, 203, 404 ms (22, 48, 97, 197, 398: the correction no longer starts about 5 ms before its note); worst formant move at +/-1 and +/-3 semitones 4.43 to 4.62% (limit 5); after a hard step the new note on pitch 0.009 to 1.74 cents at the 95th pct (limit 5; the read offset walks back 1.7 cents a grain). Full suite: 0 failures.

| Luke's 11 working takes (realvox, `_vox/rode/work/tune`) | 1.1.1 | 1.2.0 |
|---|---|---|
| Every Custom note on Bypass, output against the input at the reported delay, worst take, by Input Type | -6.1 to -8.9 dB | -332.7 dB |
| Song key, Instant: held notes 90th pct, pooled / take 02 | 3.9 / 24.2 cents | 3.3 / 2.2 |
| Song key, Instant: held notes left as sung though 20 cents off | 4 | 1 |
| Song key, default: left as sung / wrong note | 17 / 4 | 12 / 5 |
| Clicks by the shared rule, default render | 2 | 0 |

Not at zero (`_vox/rode/work/tune/REPORT.md`): take 14 at Instant with the song key, held notes 90th pct 16.5 to 35.0 cents (notes sung on an out-of-key note between two scale notes a whole tone apart, where the target flips; Tune Pro found no rule that fixes it without adding wrong notes); start lag on his takes (his entries slide into the note; entries that hold their pitch pass, see the table); the default's Humanize 30 by design (note centres); clicks with Throat Length 110 and Transpose +3 that are the take's own sharp sounds read at the grains' other speed (the shared click rule only matches copies at the same speed).

## 1.1.1, 2026-11-07

Renamed to Tune, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0, 2026-11-07

The custom window replaces the generic one. Graphite and lime: a huge note, a correction ruler in cents, a short trace on the same scale, In and Out levels, one cell per note, the main controls in view and the rest under Advanced. Every control is the same AU parameter under the same ID. The only new saved items are the window size and whether Advanced is open, kept in range when loaded.
No DSP change. Adds a read of the input and output peak for the meters. Output is bit identical to 1.0.1 through the installed AU. Adds `tests/RenderWindow.cpp`, `tests/AuRender.cpp`, pictures in `docs/window/` and window tests in `tests/PluginTest.cpp`.

## 1.0.1, 2026-11-07

Slowed the tracker's mark pull from 1% to 0.2%, which was bending shifted pitch. Worst output pitch error at instant speed: steady notes 3.44 to 1.01 cents, vibrato with breath 4.36 to 1.52 cents. Worst formant move 4.98% to 4.43%.

## 1.0.0

First version. Real time vocal pitch correction with a time domain tracker, pitch-synchronous grain shifting and formant preservation.
Controls: Key, Scale, Retune Speed, Humanize, Flex Amount, Input Type, Follow Key, Natural Vibrato, Transpose, Reference A4, Tracking, Formant Preserve, Throat Length, Custom notes (Tune, Remove, Bypass), Bypass.
Readings: Pitch Off Target, Detected Pitch, Correction Applied, Target Note, Active Key. Eight factory presets. Generic window plus a live reading strip.
