# Changelog

## 1.0.0

First version. Live key, scale and tempo detection with bit identical pass-through and no latency. Analysis on one background thread fed by a lock-free queue. Big readout window. Controls: Mode, Response, Source, Listening, Reset, Manual Key, Manual Scale, Relative Swap, Tempo Scale, Bypass. Readings exposed as read-only parameters: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo. Writes key.json for the other plug-ins. Five factory presets.

Known misses against the limits set before testing: key changes followed within 30 s (79.2% against 90%), Fast median key change time (10.30 s against 8 s), Stable clips moving after 8 s (1.46% against 1%). See README.md.
