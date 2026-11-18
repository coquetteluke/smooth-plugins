# Window CPU

One Mac (MacBook, 2x screen). Real window at 1040 by 680 points, real speech in real time, method from `~/smooth-resonance/docs/window-cpu.md`. Measure with `build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds] [scene]`. Scenes: none (LFO), `silent`, `manualrest`, `manual`, `advanced`, `feedback`, `harness`. Every run of the final build is in `window-cpu-runs.txt`.

## Final build, nothing else building

| Scene | Runs | CPU time, % of one core | Work, % of a fast core | Share on fast cores |
|---|---|---|---|---|
| LFO, default, speech | 6 | 13.5 to 14.1 | 2.6 | 1 to 3% |
| Silent, nothing moving | 3 | 2.4 to 2.5 | 0.3 | 1 to 3% |
| Manual, Flange held, speech | 3 | 12.7 | 2.1 to 2.2 | 1 to 2% |
| Manual, a hand riding Flange nonstop | 3 | 24.5 to 25.8 | 6.6 to 6.7 | 2 to 7% |
| LFO, Advanced open | 3 | 12.9 to 13.8 | 2.5 to 2.6 | 1 to 5% |
| Automation sweeping Feedback nonstop | 3 | 15.9 to 23.9 | 7.6 | 4 to 33% |

## The budget

Under about 5% of one core is met only when macOS runs the window on fast cores, and not on the quiet Mac above, where it parks the process on slow cores at low clock.

Same scene on earlier builds with the same per frame drawing, when macOS chose fast cores: 3.8, 3.9, 4.0, 4.0, 4.1 with other builds running (work 2.8), and 2.9, 3.1, 3.1, 3.1, 3.2, 4.1 with 2 to 4 builds running (work 2.8 to 2.9), 90 to 97% on fast cores. One later build read 2.8 at 97% on fast cores, in a set that also read 7.0 and 13.2 at 39 and 3%.

Control: at the same moment, on the quiet Mac, Resonance's approved window read 13.6, 13.8 and 14.3 with 2 to 4% on fast cores, and this one read 14.2 to 14.6 and then 13.0 to 14.3. Work: 3.0 for Resonance, 2.6 for this one. So the slow core reading is not specific to this window, and this window does less work.

Not met on any core: a hand riding Flange nonstop (6.6 work, 6.3 measured on fast cores on an earlier build) and automation sweeping Feedback nonstop (7.6 work, 5.8 on fast cores on an earlier build). Both last only as long as the movement does. In a real host the main thread may be scheduled differently, and that is not measured here.

## Audio CPU

`AuBench aufx STfl Luke`: default 1.0, worst preset 1.1, the same as 1.1.1. The worst single block read 2 to 5% across runs, against 3 to 4% on 1.1.1.

## What was done

- 30 frames a second at most. Nothing repaints when nothing moves.
- The comb picture is one cached image. It costs about 10 ms to work out at full size, so a parameter that keeps moving gets a half size picture about 1 ms, at most 15 times a second, and the sharp one comes 150 ms after it stops. Before that, nonstop automation read 23% work.
- Scales and labels are a separate image, so they stay sharp while the picture is at half size.
- The violet is one 480 by 300 image written by table lookup, about 144,000 lookups a frame. Two FFTs a frame barely register.
- Cell text is cached in an image. The number redraws at most 10 times a second under the mouse and 4 times otherwise. Before that, riding read 11.2% work.
- The big number is fixed width digit images, at half the frame rate, with only its own area repainted.
- The harness scene shows the test loop itself costs nothing: 6.9 million instructions a frame against 7.0 for the plain LFO scene.
