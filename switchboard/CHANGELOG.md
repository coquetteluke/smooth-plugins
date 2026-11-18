# Changelog

## 1.1.0, 2026-11-17

One Switchboard for the whole project. Put it on Stereo Out (or Master) in any free slot and it reaches your plug-ins on every track. It opens on Tracks: every track that has one of your plug-ins open, in the order Logic loaded them, each with its plug-ins, and a red "not responding" next to one that went quiet. Plug-ins Logic has not named a track for yet share a row, "No track name yet". Tap a track to see its plug-ins with their main controls, as before, with "‹ Tracks" at the top left to go back. "All N controls" on a plug-in opens every control of that plug-in, searchable, with "‹ <track>" to go back (Escape in an empty search does the same). The view menu and the All controls button of 1.0.0 are gone; the back buttons replace them.

Switchboard remembers where you were: closing and opening the window, or reopening the project, comes back to the same screen. If that track was renamed it follows it; if what it showed stays closed it goes back up, to the track or to Tracks (never in the first 10 seconds after a project opens, while its plug-ins are still loading). A session saved with 1.0.0 opens on Tracks, or on the track it had chosen.

Other programs can now see and change what the window shows, with the window open or closed, through its inbox: Switchboard reads its inbox 20 times a second and answers in its hub file.

Any channel format where input equals output: mono, stereo, LCR, quad, 5.1, 7.1, 7.1.2, Dolby Atmos 7.1.4 and the rest, so it sits on a stereo Stereo Out or a surround master. In an Atmos project put it above the Dolby Atmos plug-in. Sound unchanged, bit for bit, no latency, in every format. No animations or sounds. With the window closed it uses about half a percent of one core; open with nothing moving 1.3 to 3.5%, measured side by side with 1.0.0 on the same Mac and no higher. auval and HostCheck pass. 91 of 91 checks pass (15 for the link, 63 for Switchboard, 13 for LinkCheck on the Link Probe), 3 of 3 ctests.

## 1.0.0, 2026-11-17

The window. Switchboard now shows your plug-ins on its track, one line each with its main controls as knobs, switches and menus, and sets them: drag a knob, click a switch or a choice, double-click for the default. The knob follows your hand at once and the plug-in's own value as soon as it answers. The menu at the top shows This track (the default), All tracks or any track seen; until Logic names the track it shows all tracks, and a track with none of your plug-ins says so and offers All tracks. "All controls" opens a searchable list of every control. A plug-in that stops answering greys out with "not responding". Two of one plug-in on one track (or on two tracks with one name) show as Name and Name 2, nothing lost. The chosen view is kept with the session.

Every change moves the plug-in's own control with a begin and an end, at most 30 commands a second during a drag, so Logic records automation as from the plug-in's own window. The window reads the link folder only while open: 0.8% of one core idle and 2.1% with a control moving 8 times a second, measured on screen with six plug-ins. A scan of 200 unchanged cards takes under 1 ms. Sound unchanged, still bit for bit. auval and HostCheck pass. 74 of 74 checks pass (15 for the link, 47 for Switchboard, 12 for LinkCheck on the Link Probe), 3 of 3 ctests.

## 0.1.0, 2026-11-16

First install, a skeleton for the Logic track name check. Switchboard (`aufx SSwb Luke`) passes audio through bit for bit with no latency and has no parameters. It writes `hub-<id>.json` with the track name Logic gives it, keeps the file fresh while open and removes it when closed. The window shows the track name; the controls of your plug-ins come in 1.0.0.

Also here: `link/SmoothLink.h`, the link each plug-in will carry, and `tools/LinkStub.cpp` for end-to-end tests. auval and HostCheck pass. 44 of 44 tests pass (15 for the link, 29 for Switchboard).
