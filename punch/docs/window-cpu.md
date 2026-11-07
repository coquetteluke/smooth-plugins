# Window CPU

One Mac (MacBook, 2x screen). Real window, real speech in real time, measured with `build/RenderWindow_artefacts/Release/RenderWindow --live [runs] [seconds] [silent]`. Other agents were building on the Mac during all of these runs, so none of them meets the "nothing else building" condition. A quiet re-measure is still to come.

## Final build, 30 frames a second, 5 runs of 10 seconds, load 7.0 to 7.3, one compiler running

| Run | CPU, % of one core | Work, % of a fast core | Time on fast cores | Million instructions a frame |
|---|---|---|---|---|
| 1 | 5.1 | 2.1 | 31% | 6.2 |
| 2 | 5.8 | 2.2 | 29% | 6.5 |
| 3 | 6.1 | 2.3 | 29% | 6.6 |
| 4 | 5.8 | 2.2 | 29% | 6.3 |
| 5 | 6.7 | 2.2 | 33% | 6.4 |

Over the 5% budget on this reading, median 5.8%. The work is steady and low, but only about 30% of the time was on fast cores. Real frames came at about 27.5 a second.

Silent with nothing moving, 3 runs of 6 seconds, load 6.7 to 7.1: 1.7%, 1.7%, 1.4%. Work 0.3 to 0.4%. Almost no redraws.

## Earlier runs, all of them

- Same drawing, load 14.7 to 56.5, 5 runs: 4.9, 5.7, 6.9, 6.4, 7.0. Work 2.1 to 2.5%. Fast cores 28 to 63%. Silent, 3 runs at load 36.7 to 44.9: 1.7, 2.2, 2.1.
- Slightly looser curve thinning (0.3 pixel, no limit on run length), load 4.2 to 4.7, 4 runs: 3.3, 4.1, 3.6, 3.2. Work 2.1 to 2.2%. Fast cores 74 to 92%. This is the best case, and its fast core share is near what a host's main thread tends to get, but it is not the final build.
- Before any thinning, load 4.1 to 6.5, 6 runs: 5.9, 5.3, 6.1, 4.5, 5.3, 7.7. Work 2.3 to 2.4%. Fast cores 21 to 66%.
- First build, load 4.3 to 4.8, 5 runs: 4.7, 5.2, 5.4, 9.3, 5.2. Work 2.4 to 2.5%, except run 4 at 7.3%, which was never explained and did not come back in any later run.

## What this shows

CPU time follows which cores macOS picks, as `~/smooth-resonance/docs/window-cpu.md` found. Work, the number that does not depend on the core, stays at 2.1 to 2.5% of a fast core, under Resonance's 2.8 to 3.0%.

## Audio

`_shared/bin/AuBench aufx SPun Luke`, default: 0.7, 0.7, 0.6% on 0.2.0 and 0.6, 0.7, 0.7% on 0.3.0.
