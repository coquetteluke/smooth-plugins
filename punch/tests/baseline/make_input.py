# Writes input.f32 (48 kHz mono float32, 6.0 s) for the 0.6.1 baseline. Run once, before 0.6.2 was installed:
#   python3 make_input.py ../corpus/speech_daniel.wav input.f32
# 0.00 to 0.60  two drum-like noise hits with no pitch, the second 12 dB softer 80 ms after the first (no note yet)
# 0.60 to 3.20  real speech (macOS say, Daniel), its first 2.6 s
# 3.20 to 4.50  the room alone (hiss and a 120 Hz hum), more than a second with no note
# 4.50 to 5.70  a sung note, 196 Hz with vibrato: a loud first 100 ms, then settled 4 dB lower; then a short breathy tail
import sys, wave, numpy as np
sr = 48000
n = int(sr * 6.0)
rng = np.random.default_rng(21)
t = np.arange(n) / sr
x = 0.0003 * rng.uniform(-1, 1, n) + 0.0006 * np.sin(2 * np.pi * 120 * t)
for at, amp in ((0.10, 0.5), (0.18, 0.125)):
    i0 = int(at * sr); m = int(0.4 * sr); u = np.arange(m) / sr
    x[i0:i0 + m] += amp * np.minimum(1, u / 0.001) * np.exp(-u / 0.06) * rng.uniform(-1, 1, m)
with wave.open(sys.argv[1]) as w:
    s = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
seg = s[: int(2.6 * sr)]
i0 = int(0.6 * sr); x[i0:i0 + len(seg)] += seg
i0, i1 = int(4.5 * sr), int(5.6 * sr)
u = (np.arange(i0, i1) - i0) / sr
phase = np.cumsum(2 * np.pi * 196 * 2 ** (0.4 / 12 * np.sin(2 * np.pi * 5.5 * u)) / sr)
amp = np.where(u < 0.1, 1.0, np.where(u < 0.13, 1 - (1 - 10 ** (-4 / 20)) * (u - 0.1) / 0.03, 10 ** (-4 / 20)))
env = np.minimum(1, u / 0.01) * np.minimum(1, (1.1 - u) / 0.004)
v = sum(np.sin(h * phase) / h for h in range(1, 7))
x[i0:i1] += 0.25 * amp * env * v
i0 = i1; m = int(0.1 * sr); u = np.arange(m) / sr
x[i0:i0 + m] += 0.01 * np.exp(-u / 0.03) * rng.uniform(-1, 1, m)
x.astype('<f4').tofile(sys.argv[2])
print(n, float(np.abs(x).max()))
