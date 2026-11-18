# Switchboard

One plug-in on Stereo Out (or the master) that shows and sets the controls of your own plug-ins on every track, without moving any of them out of its slot.

Version 1.1.0. Audio passes through bit for bit with no latency, in any channel format where input equals output, mono up to Dolby Atmos 7.1.4.

## How to use it

1. Put Switchboard on Stereo Out (or Master) in any free slot: Audio FX > Audio Units > Luke Garito > Switchboard. It never touches the sound, so its place among the slots does not matter. In a Dolby Atmos project put it above the Dolby Atmos plug-in.
2. Open it. It shows Tracks: every track that has one of your plug-ins open, in the order Logic loaded them, each with its plug-ins.
3. Tap a track. It shows that track's plug-ins, one line each, with their main controls. "‹ Tracks" at the top left goes back.
4. Turn a knob: drag it up or down (hold Shift to go fine). Click a switch or a choice to change it; a menu opens with a click. Double-click a knob or a switch to put it back to its default.
5. "All 12 controls" on a line opens every control of that plug-in. Type in the search box to find one. "‹ Lead Vox" (the track's name) at the top left goes back; so does Escape in an empty search box.
6. Switchboard remembers where you were. Closing and opening the window, or reopening the project, comes back to the same screen. If that track or plug-in is no longer open, it goes back up (to the track, or to Tracks).
7. A plug-in that goes grey and says "not responding" stopped answering: Logic is busy, or the plug-in just closed. On Tracks its name greys with a red "not responding" next to it.
8. Plug-ins Logic has not named a track for yet share one row, "No track name yet".
9. It needs each plug-in's link version (the update that says "Switchboard can now see and set every control"). A plug-in without it does not show up.

Logic records what you do here as automation in Touch or Latch, exactly as from the plug-in's own window: every change moves the plug-in's real control, with a begin and an end.

A Switchboard left on a track from 1.0.0 still works, and opens on Tracks too; one is enough per project.

## The window

A dark rail on top with the back button and the name of what you are looking at, a light board below: on Tracks one white row per track, on a track one white line per plug-in, like a row of jacks on a patch bay. Each plug-in has its own cord colour (its jack, its knob arcs, its switches), the same every time; anything you press that is not a value is ink. Words are Avenir Next and numbers DIN Alternate, both part of macOS. No animations or sounds. Resizable from 600 x 420; it opens at 880 x 580.

Cost: with the window closed Switchboard passes the audio through and looks at its own inbox 20 times a second (an empty folder read). The window reads the link folder only while it is open, ten times a second, and repaints only what changed, at most 30 times a second. Nothing repaints while nothing changes. Measured on 2026-11-17 with six plug-ins on one track (`HubTest --window-cpu 20`, window on screen), on a busy Mac (Logic open at about 40% CPU), eight runs: 0.3 to 0.6% of one core with the window closed, 1.7 to 3.4% open on Tracks and 1.3 to 3.5% open on a track with nothing moving, 3.7 to 6.8% with one control moving 8 times a second (above 5% in 5 of the 8 runs). 1.0.0 measured side by side on the same Mac: 2.4 to 3.0% open, 2.5 to 8.1% moving, so 1.1.0 costs no more. A profile of the moving case shows the cost is macOS Core Animation committing each repaint, the same in both versions; Switchboard's own reading is a small part of it. Automation in Logic moves a card at most 4 times a second, half the test's rate.

## How it works

Each of your plug-ins carries `link/SmoothLink.h`. While it is open it keeps a small card of all its controls in

    ~/Library/Application Support/SmoothPlugins/link/

and reads set commands from its own inbox there. A command moves the plug-in's real control with a begin and an end, exactly as a drag in its own window does, so Logic records automation in Touch or Latch as usual. Every control stays a real Audio Unit parameter.

Logic tells each plug-in the name of its track, so a card says which track it is on. Switchboard writes `hub-<id>.json` with its own track name and the screen it shows, so readers know it is there and where it is, and takes commands that change the screen from its inbox `hub-<id>.in/`, with the window open or closed.

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
| `hub/` | The Switchboard Audio Unit (`aufx SSwb Luke`); `hub/HubFile.h` its presence file and inbox, `hub/Screen.h` what the window shows (kept by the processor, saved with the session) |
| `hub/Registry.h` | The reader: scans the folder, reads a card again only when it was replaced, alive within 5 s, responding within 2 s, groups by track in load order, numbers `Name`, `Name 2`, cleans up stale files once a minute |
| `hub/Commands.h` | The writer: `.<name>.tmp` then `<name>.json` in the plug-in's inbox, confirms by `done`, takes a command back after 3 s with no answer; `DragThrottle` (at most 30 commands a second) |
| `hub/Editor.{h,cpp}`, `hub/Controls.{h,cpp}`, `hub/Look.h` | The window, its knobs, switches and menus, and its look |
| `tests/LinkTest.cpp` | The link's tests |
| `tests/HubTest.cpp` | Switchboard's tests, with the checks every plug-in passes |
| `tools/LinkStub.cpp` | `LinkStub --dir D [--track T] [--plugin P] [--seconds N]`: a stand-in plug-in with a link, for end-to-end tests of the readers. Prints `READY <id>` |
| `tools/LinkCheck.cpp` | `LinkCheck <type> <subtype> Luke [--render-sums]`: checks an installed plug-in's link from outside, as a host loads it, or prints render sums of its default state and every preset. Copied to `~/smooth-plugins/_shared/bin/LinkCheck` by the `InstallLinkCheck` target |
| `tools/LinkProbe.cpp` | Link Probe, the test plug-in as a real Audio Unit. Never installed; LinkCheck loads it from the build folder with `--load` (ctest `linkcheck`) |

LinkCheck sets `SMOOTH_LINK_DIR` and `SMOOTH_KEY_JSON` to a fresh scratch folder itself before it loads anything. Its link check prints one PASS or FAIL line per check and ends with `LINKCHECK PASS` or `LINKCHECK FAIL`. With `--render-sums` it renders 8 s of a fixed input (and a fixed chord sequence for instruments and MIDI effects) twice per preset and prints `SUM <preset> <hash>`, or `NONDET <preset>` when the two runs differ; compare two versions' outputs with `diff`. `--state FILE` restores a saved plug-in state first, for a plug-in that is silent until something is loaded (Cutup with no sample).

`HubTest` has two modes ctest never runs: `SWITCHBOARD_RENDER_DIR=<absolute folder> HubTest` saves the painted screens there (2x: `switchboard-1.1-tracks.png`, `-track.png`, `-controls.png`, the smallest size and the empty Tracks screen), and `HubTest --window-cpu <seconds> <path to LinkStub>` runs six LinkStub plug-ins in the scratch folder and prints the CPU used with the window closed, then open on screen on Tracks, on the track, and with one control moving.

Results of the last test run are in `tests/results.txt`, changes in `CHANGELOG.md`.
