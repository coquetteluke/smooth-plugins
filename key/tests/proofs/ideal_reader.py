#!/usr/bin/env python3
# Proofs for limits that no reader of a lone voice can meet. An ideal reader here has no noise and no delay: it is handed the
# notes that were actually sung (for the made up singer, from tests/proofs/IdealNotes.cpp; for Luke's takes, the steady frames
# of the shared detector's pitch track, nearest semitone at A440) and may use any rule on them.
#
#   ideal_reader.py step [seeds]     a change of key by one and by two semitones against a singer who only strays
#   ideal_reader.py floor [session]  per take: when the sung notes first favour the song's real 7 notes, and from when they keep doing so
#
# Needs build/IdealNotes (see its first lines) and, for Luke's takes, ~/smooth-plugins/_vox/<session>/work/_detect/*.json. The takes
# themselves are never copied or committed; without them the script prints the made up singer's side only.
import json, math, os, subprocess, sys, glob
HERE = os.path.dirname(os.path.abspath(__file__)); REPO = os.path.dirname(os.path.dirname(HERE))
MAJ = (0, 2, 4, 5, 7, 9, 11)
def cover(h, r):
    t = sum(h); return sum(h[(r + k) % 12] for k in MAJ) / t if t > 0 else 0.0
# The song's real 7 notes per take, as the major key with those notes (several where the sources leave it open). TAKES.txt.
REAL = {'01': [4], '02': [4], '03': [6], '04': [11], '05': [6, 1], '06': [5], '07': [2], '08': [4], '09': [2], '10': [2], '11': [8],
        '12': [2], '13': [8], '14': [4, 11, 6], '15': [0], '16': [4], '17': [5]}
RAP = {'12', '16', '17'}
SONG = {'aint-in-la': '01', 'summertime-sadness': '02', 'god-is-a-woman': '03', 'falling-in-love': '04', 'red-ruby': '05', '34-35': '06',
        'the-girls': '07', 'henry-come-on': '08', 'francis-forever': '09', 'your-best-american-girl': '10', 'as-if-its-your-last': '11',
        'changed-it': '12', 'work-from-home': '13', 'whistle': '14', 'pink-venom': '15', 'the-light-is-coming': '16', 'love-rap': '17'}
def take_id(name, session):
    if session == 'rode': return name[:2]
    return next((i for k, i in SONG.items() if k in name), None)
def steady_notes(session, take):
    """(seconds from the first voiced frame, pitch class, seconds) for every steady frame of the pitch track."""
    d = json.load(open(os.path.expanduser(f'~/smooth-plugins/_vox/{session}/work/_detect/{take}.json'))); p = d['pitch_track_10ms']; hz = p['hz']; hop = p['hop_sec']
    first = next((i for i, f in enumerate(hz) if f > 0), 0); out = []
    for i, f in enumerate(hz):
        if f > 0 and 4 <= i < len(hz) - 4 and hz[i - 4] > 0 and hz[i + 4] > 0 and abs(12 * math.log2(hz[i - 4] / f)) <= 0.3 and abs(12 * math.log2(hz[i + 4] / f)) <= 0.3:
            out.append(((i - first) * hop, int(round(12 * math.log2(f / 440.0) + 69)) % 12, hop))
    return out
def takes(session, want_rap=False):
    for f in sorted(glob.glob(os.path.expanduser(f'~/smooth-plugins/_vox/{session}/work/_detect/*.json'))):
        name = os.path.basename(f)[:-5]; i = take_id(name, session)
        if i in REAL and (i in RAP) == want_rap: yield name, i
def floor(session):
    print(f'Floor, session {session}: an ideal reader counting every steady note from the start')
    for name, i in takes(session):
        h = [0.0] * 12; firstbest = since = None; lead = 0.0; nxt = 1.0
        for t, pc, w in steady_notes(session, name):
            h[pc] += w
            if t >= nxt and sum(h) >= 1.0:
                nxt = t + 1.0
                mine = max(cover(h, r) for r in REAL[i]); other = max(cover(h, r) for r in range(12) if r not in REAL[i])
                if mine > other:
                    if firstbest is None: firstbest = t
                    if since is None: since = t
                else: since = None
                lead = mine - other
        fm = lambda x: 'never' if x is None else '%.0f s' % x
        print(f'  {name[:30]:30s} real notes first lead at {fm(firstbest):>6s}; lead from {fm(since):>6s} to the end; lead at the end {100 * lead:+.0f} points')
def lead_after_change(notes, old, new, lo, hi):
    h = [0.0] * 12
    for t, pc, w in notes:
        if lo <= t < hi: h[pc] += w
    return cover(h, new) - cover(h, old)
def step(first=4000, count=8, window=28.0):
    exe = os.path.join(REPO, 'build', 'IdealNotes')
    print(f'A change of key at 45 s, made up singer, seeds {first} to {first + count - 1} ({24 * count} clips each). The reader knows the moment of the change')
    print(f'and hears the next {window:.0f} s (2 s are left to show the answer inside 30 s). Lead: share of the sung notes inside the new key minus inside the old key.')
    need = {}
    for semis in (1, 2):
        clips = {}
        for line in subprocess.run([exe, str(semis), str(first), str(count)], capture_output=True, text=True, check=True).stdout.splitlines():
            f = line.split(); clips.setdefault((f[1], f[2], f[3]), (int(f[4]), []))[1].append((float(f[5]), int(f[6]), float(f[7])))
        leads = sorted(lead_after_change(n, old, (old + semis) % 12, 45.0, 45.0 + window) for old, n in clips.values())
        q = lambda p: leads[int(p * (len(leads) - 1))]
        need[semis] = q(0.2)
        print(f'  up {semis}: lead of the new key, lowest {q(0):+.2f}, 20th percentile {q(0.2):+.2f}, median {q(0.5):+.2f}. To follow 80% of the clips a rule must move at a lead of {q(0.2):+.2f}.')
    for session in ('rode',):
        rows = list(takes(session))
        if not rows: print('  (Luke\'s takes are not on this machine: his side is left out)'); return
        print(f'Luke\'s sung working set takes, where the song does NOT change key: every {window:.0f} s stretch (one a second), the lead of the same move over the real key.')
        for semis in (1, 2):
            worst = []; fired = 0; n = 0
            for name, i in rows:
                if i in ('03', '08', '11', '13', '15'): continue   # the blind takes are left out of everything used to set a rule
                notes = steady_notes(session, name); end = notes[-1][0] if notes else 0.0; best = -1.0; hits = 0; t0 = 0.0
                while t0 + window <= end:
                    h = [0.0] * 12
                    for t, pc, w in notes:
                        if t0 <= t < t0 + window: h[pc] += w
                    if sum(h) >= 2.0:
                        m = min(cover(h, (r + semis) % 12) - cover(h, r) for r in REAL[i])   # against whichever accepted set is kindest
                        best = max(best, m); hits += m >= need[semis]
                    t0 += 1.0
                worst.append((best, name[:2])); fired += hits > 0; n += 1
            worst.sort(reverse=True)
            print(f'  up {semis}: a rule that moves at {need[semis]:+.2f} would move on {fired} of his {n} takes. Highest lead per take: ' + ', '.join(f'{t} {b:+.2f}' for b, t in worst))
if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'step'
    if cmd == 'floor': floor(sys.argv[2] if len(sys.argv) > 2 else 'rode')
    else: step(*(int(x) for x in sys.argv[2:4]))
