# Changelog

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
