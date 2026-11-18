#!/bin/bash
# Makes the real speech used by the window pictures and the null test, with macOS say. Run from the repo root. The results are committed, so nothing else needs say.
set -e
cd "$(dirname "$0")/corpus"
make() { # file voice text
  say -v "$2" -o "$1.aiff" "$3"
  afconvert -f WAVE -d LEI16@48000 -c 1 "$1.aiff" "$1.wav"
  rm -f "$1.aiff"
}
make speech_samantha "Samantha" "Pick it up, put it back. Take that track and make it hit harder. Bring the kick, stop, start, and go."
make speech_daniel "Daniel" "Clap your hands, tap the desk, and count it out: one, two, three, four. Back to the top, one more time."
