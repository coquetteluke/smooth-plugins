# Window CPU

Real window at 2x, one column of pixels on screen, Full Stack on `say` sung material on a real time audio thread, message thread timed in the same run. `build/RenderWindow_artefacts/Release/RenderWindow --live 5`.

Final build, 24 frames a second, every run, share of one core for the window's thread, with the audio thread and fast core share:

| Set | Run | Window | Audio | Fast cores |
|---|---|---|---|---|
| A | 1 | 4.1% | 5.3% | 18% |
| A | 2 | 2.3% | 2.5% | 53% |
| A | 3 | 1.7% | 2.0% | 78% |
| A | 4 | 2.9% | 3.1% | 45% |
| A | 5 | 4.4% | 5.3% | 28% |
| B | 1 | 3.6% | 4.0% | 39% |
| B | 2 | 2.3% | 1.8% | 89% |
| B | 3 | 4.2% | 1.8% | 88% |
| B | 4 | 2.1% | 2.1% | 87% |
| B | 5 | 2.6% | 1.8% | 83% |

Set B had other agents' builds starting on the machine by its end, and Logic open throughout, so it was not a truly quiet machine. Neither was set A. Open with nothing playing: 0.74% and 0.93%, no paints. WindowServer's share of presenting frames is not in these numbers, because it is another process.

The same runs at 30 frames a second and a 6 second view, before the last changes, read 1.3, 4.3, 2.3, then 5.4, 4.9, 6.6, 6.6, 7.5%, which is why the rate came down.

Audio: `AuBench aumf SHrm Luke` read default 1.4 to 1.5%, worst preset 1.8 to 2.9% before the window, and default 1.4%, worst preset 1.8% after.

What mattered:

- Almost all of a frame goes on the system presenting it (Core Animation commit), not on our drawing. So frame rate is the real lever. 30 down to 24 frames took the worst run from 7.5% to 4.4%. Slowing the scroll from 6 to 8 seconds across kept the motion as fine at the lower rate.
- Lines are flat one pixel columns written once into a picture used as a tape loop, not stroked paths. Only new columns are drawn.
- Only the rows the lines cover are repainted. The rows above and below do not change as the lines scroll.
- Singers are small pictures redrawn only when their note or size changes.
- Swings between runs follow which CPU cores macOS picks, as `~/smooth-resonance/docs/window-cpu.md` found. Fast core share is in each line.
