#!/bin/bash
# Makes the speech fixtures in tests/speech with macOS say. They are checked in so results stay the same when the voices change.
cd "$(dirname "$0")/speech" || exit 1
S="The quick brown fox jumps over six lazy dogs. She sells sea shells, and a big bat chased thirty fast sparrows. Please stop, take that track, and sing it softly."
say -v "Samantha" -o samantha.wav --file-format=WAVE --data-format=LEI16@48000 "$S"
say -v "Daniel" -o daniel.wav --file-format=WAVE --data-format=LEI16@48000 "$S"
