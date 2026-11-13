# The input the 1.5.4 states were rendered with (old-session proof). Mono float32 at 48 kHz, 5 s: three sung phrases
# (110 Hz, 220 to 247 Hz, 196 Hz with vibrato) made of harmonics shaped by two formants, with gaps between.
import math, struct, sys
sr = 48000
n = 5 * sr
out = [0.0] * n
def phrase(start, length, f0, f1, vib):
    ph = 0.0
    a = int(start * sr); m = int(length * sr)
    for i in range(m):
        t = i / sr
        f = f0 + (f1 - f0) * min(1.0, t / max(1e-9, length)) + vib * f0 * 0.01 * math.sin(2 * math.pi * 5.5 * t)
        ph += 2 * math.pi * f / sr
        env = min(1.0, t / 0.02) * min(1.0, (length - t) / 0.03)
        s = 0.0
        for k in range(1, 40):
            fk = k * f
            if fk > 9000: break
            g = 1.0 / k * (1 + 3 * math.exp(-((fk - 700) / 250) ** 2) + 2 * math.exp(-((fk - 1200) / 300) ** 2))
            s += g * math.sin(k * ph)
        out[a + i] += 0.08 * env * s
phrase(0.10, 1.0, 110, 110, 0)
phrase(1.70, 1.0, 220, 247, 0)
phrase(3.20, 1.2, 196, 196, 1)
sys.stdout.buffer.write(struct.pack('<%df' % n, *out))
