# Window CPU: what we learned on Smooth Resonance

One Mac (MacBook, 2x screen, 2560 by 1664). Real window, real speech in real time, other builds running. Measure with `build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds] [silent or background]`.

## Result

30 frames a second, 8 runs: 3.0% to 5.0% of one core, median 4.2%. Work is rock steady at 2.8% to 3.0% of a fast core (226 to 241 million instructions a second). Silent with nothing moving: 0.8%, and almost no redraws.

Forced onto the slow cores: 7.4% to 8.8% of one core for the same 3.0% of work. This reading cannot be brought under 5% by any drawing change. An empty window at 45 frames read 12% to 14% there.

## Why it swung from 3.2% to 17.4%

- The work never swung. Instructions per frame stay within a few percent across all of these runs.
- It is which cores macOS picks. Efficiency cores take two to three times the CPU time for the same work, and at low clock the time barely falls even with fewer frames.
- Less work can read higher. A window that drew nothing landed on slow cores and read 12% to 14%, while the full window on fast cores read 3.9%.
- The old method added noise. It subtracted a separate audio-only run, and that baseline moved from 2.4% to 6.9% between runs.
- Not the cause: 1x against 2x (real runs are all 2x), and full against partial frames. Partial repaints still cover 94% of the area, because voice and gold span most of the height, and cost does not follow area anyway.
- Still unmeasured: the window here has one column of pixels on screen, so WindowServer's compositing of a full window is not in these numbers. It is not in the process CPU in a host either.

## How to measure

- Time the threads in one run: everything in the process minus the audio thread, timed by itself.
- Set the main thread's QoS to user interactive, as a host's is. A background tool drifts onto slow cores.
- Compare windows by instructions, which do not depend on the core. Take off an audio-only instruction count, which is stable. Report the fast core share next to any CPU percentage.
- Do not trust offscreen paint time. `--cpu` says 2 ms a frame, and the real main thread spends 0.06 ms in `paint`.

## Where a real frame goes

- JUCE windows are layer backed with `drawsAsynchronously`, in a plug-in too. `paint` mostly records. CoreAnimation scan converts each filled path on its own queue (`CA::OGL::PathFiller`), then the GPU fills it. `CA::Layer::prepare_contents` looks big in `sample`, but it is the main thread waiting.
- About 7.5 million instructions a frame. About 3.3 million is overhead for a frame that draws nothing, so frame rate is the biggest lever: 45 to 30 frames took work from 4.3% to 2.9%.
- Filled area hardly matters. Edges, path fills and text do.

## What worked, in order of worth

1. Cap the frame rate. 30 is an even cadence on 60 and 120 Hz. 45 skips every fourth refresh on 60 Hz.
2. Put everything that does not move with the sound in one cached image (grid, curves, dots, handles, labels, corners). Rebuild it only when a parameter or the hover changes. Text is the most expensive thing to redraw per frame.
3. Draw gradients once into an opaque image, and cover what is above the curve with a flat fill. Never clip a gradient to a complex path.
4. Thin curves with a tolerance of a fraction of a pixel. No glow strokes, and no `strokePath` per frame. Merge paths of the same colour.
5. Do not paint behind a root with a transform: JUCE runs the parent's `paint` under it. Mark the editor opaque, which also saves clearing under every frame. Make small live readouts opaque, so they do not repaint the panel.
6. Skip the repaint when nothing moved by more than a quarter of a pixel, counting small changes until they add up. That is what makes a stopped session nearly free.
