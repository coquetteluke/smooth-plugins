# Real vocal measurements

`RealVox.cpp` renders Luke's dry vocal takes through Choir Loft and measures phrase starts, wrong first notes, breath leak, leak inside
phrases and dropouts from the input itself, since a real take has no ground truth. The measures and the pass rule are written at the top of
`RealVox.cpp`.

The takes are never copied into this repo. They live in `~/smooth-plugins/_vox/choir-loft/` (mapping in `SONGS.txt` there), and every
render, build and record file goes under `~/smooth-plugins/_vox/choir-loft/work/`.

- Dev takes, used freely: 01, 02, 03, 04, 06, 07, 09.
- Held out takes: 05, 08, 10, 11. Measured once with the version being shipped and once with the version before it, after the tool and its
  pass rule are committed. Nothing is tuned after that.

Build against this tree, and against an older one for comparison (for example `git archive <commit> | tar -x -C <dir>`):

    cmake -S realvox -B ~/smooth-plugins/_vox/choir-loft/work/build-new
    cmake -S realvox -B ~/smooth-plugins/_vox/choir-loft/work/build-old -DCHOIR_SRC=<old tree>
    nice -n 15 cmake --build <build dir> -j 2

Run from `~/smooth-plugins/_vox/choir-loft/`:

    <build>/RealVox_artefacts/Release/RealVox measure work/rec/<name>.txt 01-aint-in-la-chorus.wav ...
    <build>/RealVox_artefacts/Release/RealVox compare work/rec/<new>.txt work/rec/<old>.txt
    <build>/RealVox_artefacts/Release/RealVox demo <take.wav> work/demo/<out>.wav <from s> <secs>

`REALVOX_TRACE=<onset seconds>` prints one start frame by frame while measuring.
