# Changelog

## 0.3.1, 2026-11-07

Renamed to Throat, as Luke chose. Logic's slots cut long names off, so they all read Smooth something. The AU codes and parameters are unchanged, so saved sessions still load.

## 0.3.0

Its own window. A side view of the vocal tract as one bent tube from the cords to the lips, with the singer's throat in bone and the model in oxide red, both read live from the voice's formants. Five draggable points, a gray realistic range for width and length, Reset, a level meter, F1 to F5 and pitch before and after, the two formant curves, and a quiet latency readout. Sound is unchanged: output nulls bit for bit against 0.2.0 renders on speech, with the window closed, drawing, and being reopened during audio. Sessions from 0.2.0 load as before. The only additions to a saved session are the window's size and whether Advanced is open.

## 0.2.0

First finished version, matched from the start to the control set of the plug-in it replaces. Throat Length, Throat Width, five points along the tract each with its own width, four of them with a position, Breathiness and Breath High Pass, Source and Model Voice Type, Glottal Pulse Width, Vocal Range, Throat Precision, Mix, Output Gain, Level Match, Bypass, and a read-only Output Level. Fourteen presets. Latency 32 ms at 48 and 96 kHz, 34.8 ms at 44.1 and 88.2 kHz, dry path delay-matched. The formant engine lives in src/formant so other plug-ins can copy it.
