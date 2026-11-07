#!/bin/bash
# Renders speech through the installed AU at several settings, so one build can be compared with another using cmp.
# Usage, from the repo root: tests/au_null.sh <folder>       writes the raw output of each setting there
#                            tests/au_null.sh <a> <b>        compares two folders, sample for sample
if [ $# = 2 ]; then
  bad=0
  for f in "$1"/*.raw; do
    n=$(basename "$f")
    if cmp -s "$f" "$2/$n"; then echo "identical  $n  $(wc -c < "$f" | tr -d ' ') bytes"; else echo "DIFFERENT  $n"; bad=1; fi
  done
  exit $bad
fi
out="$1"; mkdir -p "$out"; R=build/AuRender
S=tests/corpus/speech_samantha.wav; D=tests/corpus/speech_daniel.wav
# Normalised values. Character is index / 4. Pitch runs -24 to +24, Formant Shift -12 to +12.
"$R" "$S" "$out/default.raw"
"$R" "$S" "$out/bigmove.raw" "Pitch=0.645833" "Formant Shift=0.666667"
"$R" "$D" "$out/deep.raw" "Character=0.25"
"$R" "$S" "$out/child.raw" "Character=0.5"
"$R" "$D" "$out/robot.raw" "Character=0.75"
"$R" "$D" "$out/monster.raw" "Character=1"
"$R" "$S" "$out/vibrato_whisper.raw" "Vibrato Depth=0.4" "Whisper=0.4"
"$R" "$S" "$out/bypass.raw" "Bypass=1"
