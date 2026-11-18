#!/bin/bash
# Renders real speech through the installed AU under several settings and prints a hash of each result. Run it on two builds and compare.
# Usage, from the repo root: tests/null/render.sh <output folder>. Needs build/AuRender, see tests/AuRender.cpp.
OUT="$1"; mkdir -p "$OUT"
run() { # name, then Parameter Name=normalised value...
  local name="$1"; shift
  for voice in samantha daniel; do
    build/AuRender "tests/corpus/speech_$voice.wav" "$OUT/$name-$voice.raw" "$@" > /dev/null || { echo "FAILED $name $voice"; exit 1; }
    echo "$(shasum -a 256 "$OUT/$name-$voice.raw" | cut -d' ' -f1)  $name-$voice"
  done
}
run default
run shaped "Attack=0.8" "Sustain=0.3" "Punch=0.3"
run full "Attack=0.7" "Sustain=0.65" "Punch=0.4" "Mix=0.8" "Gain=0.5" "Impact=0.6" "Ceiling=0.75" "Ceiling On=1"
run bypassed "Attack=0.8" "Impact=0.6" "Bypass=1"
