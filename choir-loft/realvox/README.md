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

## Held notes and robot or human (added for 1.5.0, Rode takes)

The second session's takes are in `~/smooth-plugins/_vox/rode/` (17 takes, `TAKES.txt` and `SETS.txt` there: working set 01 02 04 05 06 07 09 10
12 14 16, blind set 03 08 11 13 15 17). Work goes under `~/smooth-plugins/_vox/rode/work/harmony/`. Run from `~/smooth-plugins/_vox/rode/`.

    RealVox notes <records> <take.wav> ...       every held note and every pitched moment: did each voice sing it, and if not, why
    RealVox notesreport <records>                the table by lead pitch (under 98 Hz, 98 to 130 Hz, 130 Hz and up) and by cause
    RealVox notescompare <new> <old>             both tables and the pass rule H1 to H10
    RealVox robot <records> <take.wav> ...       how each voice moves against how Luke himself moves, and against Luke singing a part twice
    RealVox robotreport <records>                pooled numbers
    RealVox humanlimits <records> <at human 0> [full]   the limits U1 to U8 for the Human option, and with full the targets V1 to V4
    RealVox tracker <take.wav> <from s> <to s>   what the plug-in's tracker says each ms, beside the reference
    RealVox lowcensus <take.wav> ...             where the tracker calls a pitch under 98 Hz, and how well the sound repeated there
    RealVox wanted <take.wav> <from s> <to s>    what the note logic asks of voices 1 and 2, block by block: note, pitch and lag
    RealVox hash <take.wav> ...                  a hash of every output sample at the untouched defaults, for a bit for bit check, and the peak against the dry take's
    RealVox diff <a.f32> <b.f32>                 where two raw renders differ (written by hash with REALVOX_RAW=<folder>)

What each measures and the pass rules are in the comment blocks in `RealVox.cpp`. Settings from the environment, in every mode:
`REALVOX_PRESET=<factory preset number>`, `REALVOX_SET="inputType=3,..."` (parameter ids, each in its own units), `REALVOX_SHIFT=<samples>`
of silence in front of the take (two runs of one version then give the noise floor of every number), `REALVOX_FEED=<folder>` with
`REALVOX_FEED_DELAY=<samples>` to feed Choir Loft the file of the same name from that folder (the take through whatever sits in front of it in
a chain) while judging it against the dry take's notes.

A tree older than 1.5.0 has no tracker reason codes: build it with `-DCHOIR_SRC=<tree> -DCHOIR_OLD=ON`. The tracker then only says voiced or
not, so causes read `rough` for every reason; verdicts and shares are the same code either way.

The key of each take is found from its own pitch on the A440 grid, since the key column of `TAKES.txt` was never filled in (it reads KEY_01
and so on). Reference A4 stays at 440, as in Logic.

## The decidable start and breath rule, and the tracker census (added for 1.5.2)

`RealVox compare` and its 1.4.2 rule (T1 to T3, O1 to O9) are unchanged. At the default that rule cannot be passed or failed meaningfully:
1.4.4 against itself with the take slid by 21 or 42 ms fails 9 or 10 of its rules. `decidable.py` measures how far each measure moves by
itself and asks that a new version be no worse than the reference by more than that:

    decidable.py noise <RealVox> NOISE-default.txt <base records>=<slid or reseeded records of the same version>,<...> ...
    decidable.py rule <RealVox> NOISE-default.txt <new records> <reference records>

Slid runs: `REALVOX_SHIFT=<samples>`. Reseeded runs: `REALVOX_SEED=<n>` (1.5.2 on), which gives every random choice of the plug-in another
run without touching the take. `NOISE-default.txt` (defaults) and `NOISE-plain.txt` (Humanize and Drift 0) hold the measured noise of the
working set; the header of each says which runs it came from.

    RealVox calls <take.wav> ...                        when the tracker alone calls each start, and its time on the note by pitch; no render, 30 s for 11 takes
    RealVox onsetshape <S records> <kind> <take.wav> ...   level, pitch and periodicity through the real starts of one kind (breathy, fry, clean, consonant)
