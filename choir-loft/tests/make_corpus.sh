#!/bin/bash
# Makes the real speech and sung-style test material with macOS say. Run from the repo root. Results are committed so the tests do not need say.
set -e
cd "$(dirname "$0")/corpus"
make() { # file voice text
  say -v "$2" -o "$1.aiff" "$3"
  afconvert -f WAVE -d LEI16@48000 -c 1 "$1.aiff" "$1.wav"
  rm -f "$1.aiff"
}
LINE="She sells bright silver shells by the seashore, and every morning the whole town wakes up singing along."
make speech_samantha "Samantha" "$LINE"
make speech_daniel "Daniel" "$LINE"
make speech_kathy "Kathy" "$LINE"
make sung_goodnews "Good News" "Congratulations, you just won the whole big sweepstakes and you can go home right now."
# Long held vowels on a formant voice, with the pitch model turned off where the voice honours it
say -v Fred -o held_fred.aiff "[[pmod 0; pbas 45; rate 60]] Aaaaaaa. Eeeeeeee. Ooooooo. Aaaaaaa."
afconvert -f WAVE -d LEI16@48000 -c 1 held_fred.aiff held_fred.wav; rm -f held_fred.aiff
say -v Kathy -o held_kathy.aiff "[[pmod 0; pbas 60; rate 60]] Aaaaaaa. Eeeeeeee. Ooooooo. Aaaaaaa."
afconvert -f WAVE -d LEI16@48000 -c 1 held_kathy.aiff held_kathy.wav; rm -f held_kathy.aiff
