#!/bin/bash
# Makes the speech fixtures in tests/speech with macOS say. They are checked in so results stay the same when the voices change.
cd "$(dirname "$0")/speech" || exit 1
S="Every evening I sing the same slow song, and the sound rises and falls across the room. Oh, how we all love the warm low hum of a voice held long and open."
say -v "Samantha" -o samantha.wav --file-format=WAVE --data-format=LEI16@48000 "$S"
say -v "Daniel" -o daniel.wav --file-format=WAVE --data-format=LEI16@48000 "$S"
