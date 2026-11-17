# Switchboard

One plug-in per Logic track that shows and sets the controls of your own plug-ins on that track, without moving any of them out of its slot.

Version 0.1.0 is the first install, a skeleton: audio passes through bit for bit with no latency, and the window shows the name of the track it sits on. The controls of your plug-ins come in 1.0.0.

## Load it

Audio Units > Luke Garito > Switchboard, on any track, mono or stereo. It has no controls of its own and never changes the sound.

## How it works

Each of your plug-ins carries `link/SmoothLink.h`. While it is open it keeps a small card of all its controls in

    ~/Library/Application Support/SmoothPlugins/link/

and reads set commands from its own inbox there. A command moves the plug-in's real control with a begin and an end, exactly as a drag in its own window does, so Logic records automation in Touch or Latch as usual. Every control stays a real Audio Unit parameter.

Logic tells each plug-in the name of its track, so a card says which track it is on. Switchboard writes `hub-<id>.json` with its own track name, so readers know it is there.

`PROTOCOL.md` is the file format, which the plug-ins and Switchboard follow.

## For the developer

Build, test and install (one build at a time, `nice`):

    nice -n 15 cmake -S . -B build
    nice -n 15 cmake --build build -j 2
    ctest --test-dir build --output-on-failure
    nice -n 15 cmake --build build --target InstallAU

The tests use scratch folders under `work/scratch/` (`SMOOTH_LINK_DIR`, `SMOOTH_KEY_JSON`) and never the real link folder or key.json. Outside an Audio Unit the link is off unless `SMOOTH_LINK_DIR` holds an absolute path.

| Part | What it is |
|---|---|
| `link/SmoothLink.h` | The member side, header only. A plug-in adds the include, one member and one `updateTrackProperties` line |
| `hub/` | The Switchboard Audio Unit (`aufx SSwb Luke`) and its presence file, `hub/HubFile.h` |
| `tests/LinkTest.cpp` | The link's tests |
| `tests/HubTest.cpp` | Switchboard's tests, with the checks every plug-in passes |
| `tools/LinkStub.cpp` | `LinkStub --dir D [--track T] [--plugin P] [--seconds N]`: a stand-in plug-in with a link, for end-to-end tests of the readers. Prints `READY <id>` |
| `tools/LinkCheck.cpp` | `LinkCheck <type> <subtype> Luke [--render-sums]`: checks an installed plug-in's link from outside, as a host loads it, or prints render sums of its default state and every preset. Copied to `~/smooth-plugins/_shared/bin/LinkCheck` by the `InstallLinkCheck` target |
| `tools/LinkProbe.cpp` | Link Probe, the test plug-in as a real Audio Unit. Never installed; LinkCheck loads it from the build folder with `--load` (ctest `linkcheck`) |

LinkCheck sets `SMOOTH_LINK_DIR` and `SMOOTH_KEY_JSON` to a fresh scratch folder itself before it loads anything. Its link check prints one PASS or FAIL line per check and ends with `LINKCHECK PASS` or `LINKCHECK FAIL`. With `--render-sums` it renders 8 s of a fixed input (and a fixed chord sequence for instruments and MIDI effects) twice per preset and prints `SUM <preset> <hash>`, or `NONDET <preset>` when the two runs differ; compare two versions' outputs with `diff`. `--state FILE` restores a saved plug-in state first, for a plug-in that is silent until something is loaded (Cutup with no sample).

Results of the last test run are in `tests/results.txt`, changes in `CHANGELOG.md`.
