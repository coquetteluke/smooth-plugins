# Changelog

## 1.1.1, 2026-11-07

Renamed to Key, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 1.1.0

New window, in a light theme: paper white, black type, one red. The key and scale fill most of the window, so they read from across a room, beside a circle of fifths whose ink shows how likely each key looks right now, with how sure it is in the middle. A keyboard shows the notes of the key with the home note in red, and dots for the notes sounding now. Tuning goes red when the music is 1 Hz or more from A 440. In Manual, click the circle or the keyboard, or type a key. Advanced holds Manual Key, Manual Scale, Response and Bypass. The note about key.json failing to save stays, in red in the footer. Resizable, with the size and open section saved in the session. Audio stays bit identical and every detection reading stays bit identical to 1.0.0: the analyser only gained two read-only outputs for the window.

## 1.0.0

First version. Live key, scale and tempo detection with bit identical pass-through and no latency. Analysis on one background thread fed by a lock-free queue. Big readout window. Controls: Mode, Response, Source, Listening, Reset, Manual Key, Manual Scale, Relative Swap, Tempo Scale, Bypass. Readings exposed as read-only parameters: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo. Writes key.json for the other plug-ins. Five factory presets.

Known misses against the limits set before testing: key changes followed within 30 s (79.2% against 90%), Fast median key change time (10.30 s against 8 s), Stable clips moving after 8 s (1.46% against 1%). See README.md.
