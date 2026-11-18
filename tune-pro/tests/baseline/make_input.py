# Writes input.f32 (48 kHz mono float32, 6.0 s) for the 1.8.1 baseline. Run once, before 1.8.2 was installed:
#   python3 make_input.py ../corpus/speech_samantha.wav input.f32
# 0.00 to 0.30  room alone (hiss and a 120 Hz hum)
# 0.30 to 1.90  a sung a at 196 Hz 35 cents sharp with a +/-30 cent 5.5 Hz vibrato
# 1.90 to 2.90  a step up to 220 Hz landing 28 cents flat, held
# 2.90 to 3.30  room alone
# 3.30 to 5.80  real speech (macOS say, Samantha), its first 2.5 s
import sys, wave, numpy as np
sr = 48000
n = int(sr * 6.0)
rng = np.random.default_rng(7)
t = np.arange(n) / sr
x = 0.0003 * rng.uniform(-1, 1, n) + 0.0006 * np.sin(2 * np.pi * 120 * t)
i0, i1, i2 = int(0.3 * sr), int(1.9 * sr), int(2.9 * sr)
u = (np.arange(i0, i2) - i0) / sr
f = np.where(np.arange(i0, i2) < i1, 196 * 2 ** (35 / 1200) * 2 ** (30 / 1200 * np.sin(2 * np.pi * 5.5 * u)), 220 * 2 ** (-28 / 1200))
phase = np.cumsum(2 * np.pi * f / sr)
env = np.minimum(1, u / 0.01) * np.minimum(1, (u[-1] - u + 1 / sr) / 0.01)
# a vowel-ish spectrum: harmonics weighted by two broad peaks near 700 and 1200 Hz
v = np.zeros_like(u)
for h in range(1, 25):
    fh = h * f
    g = (1 / h) * (1 + 3 * np.exp(-((fh - 700) / 250) ** 2) + 2 * np.exp(-((fh - 1200) / 300) ** 2))
    v += g * np.sin(h * phase)
x[i0:i2] += 0.08 * env * v
with wave.open(sys.argv[1]) as w:
    s = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    if w.getframerate() != sr: raise SystemExit("rate")
seg = s[: int(2.5 * sr)]
i0 = int(3.3 * sr); x[i0:i0 + len(seg)] += seg
x.astype('<f4').tofile(sys.argv[2])
print(n, float(np.abs(x).max()))
