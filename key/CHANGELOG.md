# Changelog

## 1.3.0, 2026-11-07

Steadier and more often right. 1.2.0 and 1.3.0 were run on the same new held out seed, 20261110, which nothing was tuned on:

| | 1.2.0 | 1.3.0 | Limit |
|---|---|---|---|
| Stable clips moving after 8 s | 2.50% | **0.83%** | up to 1% |
| Chords exact | 97.5% | 99.4% | at least 90% |
| Key change followed in 30 s, Stable | 95.8% | 100% | at least 90% |
| Key change followed in 30 s, Fast | 87.5% | 87.5% | at least 90% |
| Key change median, Fast | 10.30 s | 9.11 s | up to 8 s |
| Melody only, Full Mix | 95.8% | 100% | none |
| Chords at 44.1 and 96 kHz | 97.9% | 100% | at least 90% |

Bold now passes. Fast still misses its two limits. Most of its misses are a minor loop of i, iv, VII, III, which is the same chords as vi, ii, V, I in the relative major, and Fast drifts to that reading later on.

- A note can now be heard as odd harmonics only, as from a clarinet or a hollow synth. Before, its third harmonic was heard as a fifth above it, which pulled readings to the key a fifth up.
- Notes clearly outside a key's scale now count against that key. The listener profiles barely minded them, and one outside note is often all that tells two keys a fifth apart.
- Stable's first reading waits while the leader is close to another key, its relative included, instead of guessing at 8 s and correcting later. Most clips still read at 7.9 s.
- Tuned on dev seeds 1000 to 1005 only, now with all 12 keys per seed. New detection fingerprint.
- The analysis thread does about four times the note solving work. It runs off the audio thread, so audio is unchanged.

## 1.2.0, 2026-11-07

Better at following a key change. Both versions were run on the same new held out seed, 20261109, which nothing was tuned on:

| | 1.1.1 | 1.2.0 | Limit |
|---|---|---|---|
| Key change followed in 30 s, Stable | 75.0% | **91.7%** | at least 90% |
| Key change median, Stable | 12.0 s | 10.1 s | up to 15 s |
| Key change followed in 30 s, Fast | 62.5% | 83.3% | at least 90% |
| Key change median, Fast | 9.62 s | **6.21 s** | up to 8 s |
| Stable clips moving after 8 s | 1.67% | 2.29% | up to 1% |
| Chords exact | 95.4% | 97.9% | at least 90% |
| Loop heard from its second chord | 67.5% | 66.3% | none |

Bold now passes. Two limits still fail, and one of them got worse: 11 of 480 Stable clips move after 8 s, against 8 before.

- When the notes leave the key, it finds where the new notes began. What was heard since then stays whole, and only what came before is mostly dropped. Before, everything was cut to 15%, the new notes too.
- The chord at that point is heard as the opening of the new section, which helps tell the new key from its relative.
- A chord change not heard lately, after a settled loop, also counts as a new section.
- Right after a key change the reading may correct itself sooner, while the new memory fills.
- A little more trust in the opening chord when there is a bass line. More than this made Stable readings move after 8 s.
- Tuned on dev seeds 1000 to 1002 only. Every detection reading has a new fingerprint, as this changes detection on purpose.
- Test fix: the window test waited a fixed 100 ms for the Manual key and failed most runs, in 1.1.1 too. It now waits up to 2 s.

## 1.1.1, 2026-11-07

Renamed to Key, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

New window, in a light theme: paper white, black type, one red. The key and scale fill most of the window, so they read from across a room, beside a circle of fifths whose ink shows how likely each key looks right now, with how sure it is in the middle. A keyboard shows the notes of the key with the home note in red, and dots for the notes sounding now. Tuning goes red when the music is 1 Hz or more from A 440. In Manual, click the circle or the keyboard, or type a key. Advanced holds Manual Key, Manual Scale, Response and Bypass. The note about key.json failing to save stays, in red in the footer. Resizable, with the size and open section saved in the session. Audio stays bit identical and every detection reading stays bit identical to 1.0.0: the analyser only gained two read-only outputs for the window.

## 1.0.0

First version. Live key, scale and tempo detection with bit identical pass-through and no latency. Analysis on one background thread fed by a lock-free queue. Big readout window. Controls: Mode, Response, Source, Listening, Reset, Manual Key, Manual Scale, Relative Swap, Tempo Scale, Bypass. Readings exposed as read-only parameters: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo. Writes key.json for the other plug-ins. Five factory presets.

Known misses against the limits set before testing: key changes followed within 30 s (79.2% against 90%), Fast median key change time (10.30 s against 8 s), Stable clips moving after 8 s (1.46% against 1%). See README.md.
