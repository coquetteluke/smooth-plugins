# Changelog

## 1.0.0

First version. Live key, scale and tempo detection with bit identical pass-through and no latency. Analysis on one background thread fed by a lock-free queue. Big readout window. Controls: Mode, Response, Source, Listening, Reset, Manual Key, Manual Scale, Relative Swap, Tempo Scale, Bypass. Readings exposed as read-only parameters: Detected Key, Detected Scale, Confidence, Reference Pitch, Detected Tempo. Writes key.json for the other plug-ins. Five factory presets.
