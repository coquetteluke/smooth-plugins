# Changelog

## 0.1.0, 2026-11-16

First install, a skeleton for the Logic track name check. Switchboard (`aufx SSwb Luke`) passes audio through bit for bit with no latency and has no parameters. It writes `hub-<id>.json` with the track name Logic gives it, keeps the file fresh while open and removes it when closed. The window shows the track name; the controls of your plug-ins come in 1.0.0.

Also here: `link/SmoothLink.h`, the link each plug-in will carry, and `tools/LinkStub.cpp` for end-to-end tests. auval and HostCheck pass. 44 of 44 tests pass (15 for the link, 29 for Switchboard).
