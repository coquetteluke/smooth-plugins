# Link protocol, version 1

How the plug-ins and Switchboard share controls. Two programs implement it:
`link/SmoothLink.h` (inside each plug-in, the "member") and the Switchboard AU (reads cards, writes
commands). Any other program that follows this file can do what Switchboard does.
This file is the contract. A change here is a change to both, and `"link"` goes up by one.

## Folder

- `$SMOOTH_LINK_DIR` when it holds an absolute path (tests, always), else
  `~/Library/Application Support/SmoothPlugins/link/` (home from `getpwuid`, as `KeyFile.h` does).
- Members create it (mode 0755) if missing. Nobody else ever deletes it.

## Members (one per plug-in instance)

- `id`: 16 lowercase hex characters, random per instance, never saved in the plug-in state
  (a copied slot must not share an id). Not stable across a project reload; refer to an instance
  by track + plug-in name, never by id across sessions.
- Card: `<id>.json`. Inbox: `<id>.in/` (a folder).
- Heartbeat is the card's modification time: the member rewrites the card or touches it
  (`utimes`) at least once a second. A card whose mtime is more than 5 s old is "not responding";
  readers hide it after 5 s and may delete card and inbox once it is 600 s old.
- On destruction the member deletes its card, everything in its inbox, and the inbox.
- On start a member deletes cards (and their inboxes) and `hub-*.json` files older than 600 s,
  and inboxes with no card older than 600 s. Nothing younger is ever touched by anyone but its owner.

### Card `<id>.json`

Written atomically: write `<id>.json.tmp<random>` in the same folder, then `rename`. Readers
never see a half file. UTF-8 JSON, one object:

```json
{
  "link": 1,
  "id": "9f2c0a71d4e3b8c6",
  "plugin": "Tune Pro",
  "type": "aumf", "subtype": "STpr", "manufacturer": "Luke",
  "version": "1.9.0",
  "track": "Lead Vox",
  "pid": 41234,
  "started": 1794430217.123,
  "seq": 42,
  "params": [
    { "id": "retune", "index": 0, "name": "Retune Speed",
      "value": 0.03, "text": "3 ms", "default": 0.2,
      "steps": 0, "choices": [], "label": "ms",
      "automatable": true, "main": true }
  ],
  "done": [
    { "cmd": "1794430230456-test-a1b2c3", "ok": true, "param": "retune",
      "value": 0.03, "text": "3 ms", "error": "" }
  ]
}
```

- `track`: the name Logic gives the plug-in (`kAudioUnitProperty_ContextName`, which JUCE hands
  to `AudioProcessor::updateTrackProperties`), or `null` until the host sends one.
- `seq`: goes up by one on every write. `started`: unix seconds, for ordering two instances of one
  plug-in on one track (older first).
- `params`: every parameter of `getParameters()`, in that order. `id` is the JUCE parameter ID
  (`HostedAudioProcessorParameter::getParameterID()`), or `"#<index>"` for a parameter without one.
  `value` and `default` are normalized 0..1; `text` is `getText(value, 64)` with the label as the
  plug-in shows it; `steps` is `getNumSteps()` when the parameter is discrete, else 0; `choices` is
  `getAllValueStrings()` for a discrete parameter that has names, else `[]`; `main` marks the few
  controls the plug-in names as its main ones (shown first by Switchboard).
- `done`: the last 16 commands applied, newest last. The card is rewritten at once after a command
  is applied (not on the debounce), so a writer sees its result within one poll.
- Value changes from anywhere (Logic automation, the plug-in window, a command) mark the card
  dirty from any thread with an atomic flag only. The card is written from the message thread,
  at most 4 times a second while values keep changing.

### Inbox `<id>.in/` and commands

- A writer creates `<id>.in/.<name>.tmp`, writes it, then renames it to `<id>.in/<name>.json`.
  `<name>` is `<unix ms>-<writer>-<6 random hex>`, writer such as `switchboard` or `test`.
  Files starting with `.` are never read.
- The member polls its inbox from the message thread about 30 times a second, applies the
  `*.json` files in name order (at most 64 per poll), deletes each one, and records each in `done`.
- A command is one object:

```json
{ "param": "retune", "value": 0.5, "gesture": "tap", "from": "test" }
{ "param": "retune", "text": "3 ms" }
```

  - `param`: a parameter `id` from the card (required).
  - Exactly one of `value` (normalized, clamped to 0..1) or `text` (through `getValueForText`;
    for a parameter with `choices` the choice name, case-insensitive; `"default"` means the
    parameter's default).
  - `gesture`: `tap` (default) = begin gesture, set, end gesture. For a drag: `begin` (begin, set),
    `move` (set), `end` (set, end). A gesture left open 2 s without a move or end is ended by the
    member.
  - Setting is `beginChangeGesture` / `setValueNotifyingHost` / `endChangeGesture` on the message
    thread: exactly what a mouse drag in the plug-in's own window does, so Logic's automation in
    Touch or Latch records it.
- A file that is not valid JSON, names an unknown `param`, or has neither or both of
  `value`/`text` is deleted and recorded in `done` with `"ok": false` and a plain `error`.

## Switchboard presence `hub-<id>.json`

Each Switchboard instance writes `hub-<id>.json` (same atomic write, same mtime heartbeat):
`{ "link": 1, "hub": true, "id": "...", "track": "Lead Vox" | null, "version": "1.0.0" }`.
Readers of members skip every file whose name starts with `hub-`.

## Reading rules (Switchboard and other readers)

- Only files matching `^[0-9a-f]{16}\.json$` are member cards. Ignore `.tmp` files and anything
  else. A card that fails to parse is skipped this round (never deleted by a reader under 600 s).
- Group members by `track` (exact string). Two instances of one plug-in on one track are
  `Name` and `Name 2` by `started`.
- A set is confirmed when the card's `done` holds the command name. No confirmation in 3 s is a
  failure ("the plug-in did not answer"), never a silent success.
