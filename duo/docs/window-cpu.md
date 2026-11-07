# Window CPU: Duo

Measured with `build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds] [silent]`: the real window at the screen's 2x, Loose Gang (three doubles moving), sung material in real time, main thread at user interactive QoS. Other agents were building throughout, load 9 to 11, so this is not a quiet machine.

## Final build, 30 frames a second

| Run | Window, % of one core | Fast core share | Fast core equivalent |
|---|---|---|---|
| 1 | 4.0 | 75% | 1.4 |
| 2 | 4.4 | 75% | 1.3 |
| 3 | 3.2 | 93% | 1.7 |
| 4 | 2.4 | 91% | 1.4 |
| 5 | 2.7 | 89% | 1.4 |

Three earlier runs, same drawing with a smaller mic dot, landed mostly on slow cores: 3.1, 4.7 and 6.0 (78%, 30% and 10% fast core), with the same 1.3 to 1.4 of work. Slow cores are the only reason any run reads above 5.

Silent, with audio still running: 0.9 and 1.2 of one core, no paints at all.

## What moved the number

- About 3.8 million instructions a frame, against 3.3 million for a frame that draws nothing.
- **One merged repaint rectangle per frame took it from 10.4 million to 3.6 million instructions a frame.** With one `repaint (rect)` per singer, the same drawing cost three times as much. Singers at opposite ends make that rectangle most of the stage, and it is still far cheaper. Area does not matter, the number of separate dirty rectangles does.
- Singers are images, one per angle they can turn to, drawn when first needed. Paths took it from 11 to 10.4 million.
- The stage, rows, labels and seats are one cached image, redrawn only on resize. Lanes and marks are a copy of it with those drawn on top, redrawn only when a setting changes.
- Text that changes (delays, note) is in small opaque readouts, updated 5 times a second, and only when the text differs.
- At rest a 125 ms timer looks for sound or a changed setting. The screen's refresh only drives the window while something moves.

## Audio

The window's one addition to the audio path is reading the input peak: 870 instructions a block, 0.002% of a fast core. `AuBench aufx SDuo Luke`: 0.9% default and 1.9% worst preset before the window, 0.9% and 1.8% after. Under heavy load from other builds it read as high as 1.9% and 4.3%, which is the other builds.
