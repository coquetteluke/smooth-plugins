#!/bin/bash
# Renders speech through the installed AU at many settings, so one build can be compared with another.
# Usage, from the repo root: tests/au_null.sh <folder>       writes the raw output of each setting there
#                            tests/au_null.sh <a> <b>        compares two folders, sample for sample
# tests/au_baseline.sha256 holds the checksums of the renders from installed 0.2.1. Compare a new folder with: (cd folder && shasum -a 256 -c ../tests/au_baseline.sha256)
if [ $# = 2 ]; then
  bad=0
  for f in "$1"/*.raw; do
    n=$(basename "$f")
    if cmp -s "$f" "$2/$n"; then echo "identical  $n  $(wc -c < "$f" | tr -d ' ') bytes"; else echo "DIFFERENT  $n"; bad=1; fi
  done
  exit $bad
fi
# From 0.4.0: set SETTLE=1 to start the plug-in again once the settings are in (the render then begins AT the settings), and
# COMPONENT=<path to a .component> to render a build kept aside in place of the installed one. tests/au_baseline_settled.sha256 holds the
# checksums of 0.3.1 made with SETTLE=1. The older tests/au_baseline.sha256 was made without it: there every control glides from its default
# to its setting as the render starts, and since 0.4.0 a control that glides arrives exactly, which 0.3.1 did not. See tests/au_null_results.txt.
out="$1"; mkdir -p "$out"; R=build/AuRender
EXTRA=(); [ -n "$SETTLE" ] && EXTRA+=("settle=1"); [ -n "$COMPONENT" ] && EXTRA+=("component=$COMPONENT")
L=tests/speech/samantha.wav; D=tests/speech/daniel.wav
for p in 0 1 2 3 4 5 6 7 8 9 10 11 12; do "$R" "$L" "$D" "$out/preset$p.raw" "preset=$p" "${EXTRA[@]}" || exit 1; done
ALL=("Voices=1" "Rate=0.3" "Depth=0.4" "Delay Time=0.2" "Feedback=0.7" "Wow=0.5" "Wow Rate=0.4" "Flutter=0.6" "Flutter Rate=0.4" "Random=0.5" "Random Rate=0.5" "Unsync=1" "Saturation=0.6" "Degrade=0.5" "Degrade Rate=0.5" "Noise=0.4" "Noise Gate=0" "Low Cut=0.3" "High Cut=0.6" "Tone=0.4" "Spread=0.9" "Mix=0.6" "Input Gain=0.55" "Output Gain=0.45")
"$R" "$L" "$D" "$out/all_s48b128.raw" "${ALL[@]}" "${EXTRA[@]}" || exit 1
"$R" "$L" "$D" "$out/all_s44b64.raw" rate=44100 block=64 "${ALL[@]}" "${EXTRA[@]}" || exit 1
"$R" "$L" "$D" "$out/all_s96b512.raw" rate=96000 block=512 "${ALL[@]}" "${EXTRA[@]}" || exit 1
"$R" "$L" "$D" "$out/all_m48b256.raw" channels=1 block=256 "${ALL[@]}" "${EXTRA[@]}" || exit 1
"$R" "$L" "$D" "$out/default_m48b256.raw" channels=1 block=256 "${EXTRA[@]}" || exit 1
"$R" "$L" "$D" "$out/bypass.raw" "Bypass=1" "${EXTRA[@]}" || exit 1
