#!/usr/bin/env python3
# The decidable start and breath rule (1.5.2), beside the 1.4.2 rule in RealVox.cpp (T1 to T3, O1 to O9), which stays as it is.
#
# The 1.4.2 rule asks three measures to be strictly better and the rest to be no worse within a fixed slack. On a real take every one of
# those measures moves by itself when nothing about the plug-in changes: slide the take by a few ms and the 2 ms scan, the 20 ms hold and
# the plug-in's random choices all land elsewhere. One version against itself fails that rule. So it cannot be passed or failed
# meaningfully at the default. This rule can: each measure must be no worse than the reference by more than that measure's own noise.
#
#   decidable.py noise <RealVox binary> <out file> <base records>=<same version, slid or reseeded>,<...> [<base>=<...> ...]
#       the noise of each measure: the largest distance from the base run that any slid or reseeded run of the SAME version shows
#   decidable.py rule <RealVox binary> <noise file> <new records> <reference records>
#       PASS or FAIL per measure: new <= reference + noise (every measure here is better when lower), and the result
#
# Records are what RealVox measure writes. Slid: REALVOX_SHIFT=<samples>. Reseeded: REALVOX_SEED=<n> (1.5.2 on), which moves every random
# choice of the plug-in without touching the take.
import sys, subprocess, re

MEASURES = [  # (name in the 1.4.2 rule, key)
    ('T1 breath leak share', 'breath leak'), ('T2 phrase starts with a wrong first note, share', 'phrase wrong'),
    ('T3 breathy starts, phrase and word, 90th percentile (ms)', 'any breathy p90'),
    ('O1 phrase starts missed, share', 'phrase missed'), ('O2 phrase starts median (ms)', 'phrase median'), ('O3 phrase starts 90th percentile (ms)', 'phrase p90'),
    ('O4 clean phrase starts median (ms)', 'phrase clean median'), ('O4 clean phrase starts 90th percentile (ms)', 'phrase clean p90'),
    ('O4 phrase starts after a consonant, median (ms)', 'phrase consonant median'), ('O4 phrase starts after a consonant, 90th percentile (ms)', 'phrase consonant p90'),
    ('O4 fry phrase starts median (ms)', 'phrase fry median'), ('O4 fry phrase starts 90th percentile (ms)', 'phrase fry p90'),
    ('O4 breathy phrase starts median (ms)', 'phrase breathy median'),
    ('O5 word starts median (ms)', 'word median'), ('O5 word starts 90th percentile (ms)', 'word p90'), ('O5 word starts missed, share', 'word missed'),
    ('O5 word starts with a wrong first note, share', 'word wrong'),
    ('O6 dropout share', 'dropout'), ('O7 leak inside phrases, share', 'in phrase leak'), ('O8 breath leak longest run (ms)', 'breath longest'),
    ('O9 breath leak energy (dB)', 'breath energy'),
]

def read(binary, path):
    out = subprocess.run([binary, 'report', path], capture_output=True, text=True).stdout
    r = {}
    for l in out.splitlines():
        m = re.match(r'\s+(any|phrase|word)( \w+)?\s+measured\s+(\d+).*missed\s+(\d+)\s+wrong first note\s+(\d+).*median\s+([\d.]+)\s+p90\s+([\d.]+)', l)
        if m:
            k = (m.group(1) + (m.group(2) or '')).strip(); n = max(1, int(m.group(3)))
            r[k + ' median'] = float(m.group(6)); r[k + ' p90'] = float(m.group(7)); r[k + ' missed'] = int(m.group(4)) / n; r[k + ' wrong'] = int(m.group(5)) / n
        m = re.match(r'\s+breath leak: (\d+) frames, voice sings in ([\d.]+), longest (\d+) ms, energy ([-\d.]+)', l)
        if m: r['breath leak'] = float(m.group(2)); r['breath longest'] = float(m.group(3)); r['breath energy'] = float(m.group(4))
        m = re.match(r'\s+in phrase leak: (\d+) frames, voice sings in ([\d.]+)', l)
        if m: r['in phrase leak'] = float(m.group(2))
        m = re.match(r'\s+dropouts: (\d+) frames, ([\d.]+)', l)
        if m: r['dropout'] = float(m.group(2))
    return r

def main():
    mode, binary = sys.argv[1], sys.argv[2]
    if mode == 'noise':
        noise = {k: 0.0 for _, k in MEASURES}; runs = 0; lines = []
        for group in sys.argv[4:]:
            base, others = group.split('=')
            b = read(binary, base)
            for o in others.split(','):
                v = read(binary, o); runs += 1
                for _, k in MEASURES:
                    d = abs(v[k] - b[k]); noise[k] = max(noise[k], d)
                lines.append('# ' + o + ' against ' + base + ': ' + ' '.join(f"{v[k] - b[k]:+.4g}" for _, k in MEASURES))
        with open(sys.argv[3], 'w') as f:
            f.write(f"# Noise of each start and breath measure: the largest distance from the base run over {runs} slid or reseeded runs of the same version.\n")
            f.write("# Written by realvox/decidable.py noise. One line per measure: key, noise.\n")
            for name, k in MEASURES: f.write(f"{k}\t{noise[k]:.6g}\t{name}\n")
            f.write('\n'.join(lines) + '\n')
        for name, k in MEASURES: print(f"{name:62s} noise {noise[k]:.4g}")
    elif mode == 'rule':
        noise = {}
        for l in open(sys.argv[3]):
            if l.startswith('#') or not l.strip(): continue
            f = l.rstrip('\n').split('\t'); noise[f[0]] = float(f[1])
        n, o = read(binary, sys.argv[4]), read(binary, sys.argv[5])
        failed = 0
        for name, k in MEASURES:
            ok = n[k] <= o[k] + noise[k] + 1e-12
            failed += 0 if ok else 1
            print(f"{'PASS' if ok else 'FAIL'}  D {name}: new {n[k]:.4f}, reference {o[k]:.4f}, noise {noise[k]:.4f}")
        print(f"RESULT {'PASS' if failed == 0 else 'FAIL'}  {failed} of the {len(MEASURES)} decidable rules failed")
    else:
        print(__doc__); sys.exit(2)

if __name__ == '__main__':
    main()
