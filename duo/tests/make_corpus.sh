#!/bin/bash
# Makes the real speech and sung-style test material with macOS say. Run from the repo root. Results are committed so the tests do not need say.
set -e
cd "$(dirname "$0")/corpus"
make() { # file voice text
  say -v "$2" -o "$1.aiff" "$3"
  afconvert -f WAVE -d LEI16@48000 -c 1 "$1.aiff" "$1.wav"
  rm -f "$1.aiff"
}
make speech_samantha "Samantha" "She sells bright silver shells by the seashore, and every morning the whole town wakes up singing along."
make sung_goodnews "Good News" "Congratulations, you just won the whole big sweepstakes and you can go home right now."
