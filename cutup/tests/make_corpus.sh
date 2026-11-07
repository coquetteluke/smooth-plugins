#!/bin/bash
# Makes the speech the window tool loads, with macOS say. Run from the repo root.
set -e
out=tests/corpus
mkdir -p "$out"
say -v Samantha -o /tmp/smooth-slice-say.aiff "Cut the vocal into pieces, put them on the keys, and play it back your own way."
afconvert -f WAVE -d LEI16@48000 -c 1 /tmp/smooth-slice-say.aiff "$out/say-phrase.wav"
rm -f /tmp/smooth-slice-say.aiff
ls -l "$out"
