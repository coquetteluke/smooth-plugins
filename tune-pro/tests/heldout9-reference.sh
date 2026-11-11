#!/bin/bash
# The ninth held out set, speaking voices, case by case: is "output against the input, 4%" a limit a perfect shifter could meet?
# For every case and band it tables four readings from the suite's own tool (shapeScale, tests/Analysis.h), as percent from 1:
#   out/in    the plug-in's output against the input              (what the test limits to 4%)
#   out/real  the output against a real voice with the same vocal tract at the new pitch
#   in/real   the input against that real voice                   (the suite's "floor")
#   real/in   that real voice itself against the input            (what a perfect shifter would read on the test as written)
# Usage: tests/heldout9-reference.sh [path to PluginTest] > tests/heldout9-reference.txt
T=${1:-build/PluginTest_artefacts/Release/PluginTest}
TUNE_DUMP=1 nice -n 15 "$T" heldout9 | python3 -c '
import re, sys
rows = []
tail = []
for l in sys.stdin:
    m = re.search(r"vowel (.+) around (\d+) Hz, shift (-?\d+), band (\d+) to (\d+) Hz: against input ([\d.]+), against a real voice there ([\d.]+), input against that voice ([\d.]+), that real voice against input ([\d.]+)", l)
    if m:
        rows.append((m[1], int(m[2]), int(m[3]), m[4] + " to " + m[5]) + tuple(abs(float(m[i]) - 1) * 100 for i in (6, 7, 8, 9)))
    elif "ninth held out set" in l or l.startswith(("PASS    ", "FAIL    ", "INFO    ")) or "[HQ]   " in l:
        tail.append(l.rstrip())
speak = [r for r in rows if r[1] < 300]
print("Ninth held out set, speaking voices: %d cases x 2 bands = %d readings. Percent away from 1.000; the tool steps in 0.2%%." % (len(speak) // 2, len(speak)))
print("%-28s %4s %5s %-13s %7s %8s %7s %7s" % ("vowel", "Hz", "shift", "band Hz", "out/in", "out/real", "in/real", "real/in"))
for r in speak:
    print("%-28s %4d %+5d %-13s %7.1f %8.1f %7.1f %7.1f%s" % (r + (("  <- over 4 on the test as written" if r[4] > 4 else "") + ("  <- a perfect shifter is over 4 here" if r[7] > 4 else ""),)))
w = lambda i: max(speak, key=lambda r: r[i])
print()
for name, i in (("out/in  (the test as written, limit 4)", 4), ("out/real (output against a real voice at the new pitch)", 5), ("in/real (the floor)", 6), ("real/in (a perfect shifter on the test as written)", 7)):
    r = w(i)
    print("worst %-58s %.1f%%  %s, %d Hz, shift %+d, band %s" % (name, r[i], r[0], r[1], r[2], r[3]))
print("readings over 4%%: out/in %d, out/real %d, in/real %d, real/in %d of %d" % tuple([sum(r[i] > 4 for r in speak) for i in (4, 5, 6, 7)] + [len(speak)]))
print("readings where the output is nearer the real voice than the input is (out/real < in/real): %d; further: %d; equal: %d" % (sum(r[5] < r[6] for r in speak), sum(r[5] > r[6] for r in speak), sum(r[5] == r[6] for r in speak)))
print()
print("The suite lines for this set from the same run:")
for l in tail: print(l)
'
