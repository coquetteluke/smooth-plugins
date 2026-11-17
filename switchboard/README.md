# Switchboard

One plug-in per Logic track that shows and sets the controls of your own plug-ins on that track, without moving any of them out of its slot.

Version 1.0.0. Audio passes through bit for bit with no latency; the window shows your plug-ins and sets their controls.

## How to use it

1. Put Switchboard on a track: Audio FX > Audio Units > Luke Garito > Switchboard. Any slot, mono or stereo. It never changes the sound.
2. Open it. It shows your plug-ins on that track, one line each, with their main controls.
3. Turn a knob: drag it up or down (hold Shift to go fine). Click a switch or a choice to change it; a menu opens with a click.
4. Double-click a knob or a switch to put it back to its default.
5. "All 12 controls" on a line (or All controls at the top) lists every control of that plug-in (or of every plug-in shown). Type in the search box to find one.
6. The menu at the top says This track. Pick All tracks, or any other track that has your plug-ins on it.
7. A line that goes grey and says "not responding" is a plug-in that stopped answering: Logic is busy, or the plug-in just closed.
8. It needs each plug-in's link version (the update that says "Switchboard can now see and set every control"). A plug-in without it does not show up.

Logic records what you do here as automation in Touch or Latch, exactly as from the plug-in's own window: every change moves the plug-in's real control, with a begin and an end.

## The window

A dark rail on top with the view menu and All controls, a light board below with one white line per plug-in, like a row of jacks on a patch bay. Each plug-in has its own cord colour (its jack, its knob arcs, its switches), the same every time; anything you press that is not a value is ink. Words are Avenir Next and numbers DIN Alternate, both part of macOS. Resizable from 600 x 420; it opens at 880 x 580.

Cost: the window reads the link folder only while it is open, ten times a second, and repaints only what changed, at most 30 times a second. Measured on 2026-11-17 with the window on screen and six plug-ins on the track (`HubTest --window-cpu 30`): 0.8% of one core with nothing moving, 2.1% with one control moving 8 times a second (an earlier run: 1.0% and 2.7%). Nothing repaints while nothing changes. With the window closed Switchboard does nothing but pass the audio through.

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
| `hub/Registry.h` | The reader: scans the folder, reads a card again only when it was replaced, alive within 5 s, responding within 2 s, groups by track, numbers `Name`, `Name 2` |
| `hub/Commands.h` | The writer: `.<name>.tmp` then `<name>.json` in the plug-in's inbox, confirms by `done`, takes a command back after 3 s with no answer; `DragThrottle` (at most 30 commands a second) |
| `hub/Editor.{h,cpp}`, `hub/Controls.{h,cpp}`, `hub/Look.h` | The window, its knobs, switches and menus, and its look |
| `tests/LinkTest.cpp` | The link's tests |
| `tests/HubTest.cpp` | Switchboard's tests, with the checks every plug-in passes |
| `tools/LinkStub.cpp` | `LinkStub --dir D [--track T] [--plugin P] [--seconds N]`: a stand-in plug-in with a link, for end-to-end tests of the readers. Prints `READY <id>` |
| `tools/LinkCheck.cpp` | `LinkCheck <type> <subtype> Luke [--render-sums]`: checks an installed plug-in's link from outside, as a host loads it, or prints render sums of its default state and every preset. Copied to `~/smooth-plugins/_shared/bin/LinkCheck` by the `InstallLinkCheck` target |
| `tools/LinkProbe.cpp` | Link Probe, the test plug-in as a real Audio Unit. Never installed; LinkCheck loads it from the build folder with `--load` (ctest `linkcheck`) |

LinkCheck sets `SMOOTH_LINK_DIR` and `SMOOTH_KEY_JSON` to a fresh scratch folder itself before it loads anything. Its link check prints one PASS or FAIL line per check and ends with `LINKCHECK PASS` or `LINKCHECK FAIL`. With `--render-sums` it renders 8 s of a fixed input (and a fixed chord sequence for instruments and MIDI effects) twice per preset and prints `SUM <preset> <hash>`, or `NONDET <preset>` when the two runs differ; compare two versions' outputs with `diff`. `--state FILE` restores a saved plug-in state first, for a plug-in that is silent until something is loaded (Cutup with no sample).

`HubTest` has two modes ctest never runs: `SWITCHBOARD_WINDOW_PNG=<absolute path> HubTest` saves the painted window (2x, with 1x, all tracks, All controls, smallest and empty versions next to it), and `HubTest --window-cpu <seconds> <path to LinkStub>` opens the window on screen with six LinkStub plug-ins in the scratch folder and prints the CPU it used.

Results of the last test run are in `tests/results.txt`, changes in `CHANGELOG.md`.
