"""dsp.py - shared resampling / quantisation helpers for the harness."""
import sys
try:
    import numpy as np
    from scipy.signal import firwin, resample_poly
except ImportError as e:
    sys.exit(f"error: missing dependency ({e}); install: "
             f"pip install --break-system-packages numpy scipy")

# target rate (Msps) -> (up, down) from 8 Msps
RATES = {"2.0": (1, 4), "2.4": (3, 10), "3.2": (2, 5)}


def design(up, down):
    """Anti-alias filter (same design scipy's resample_poly uses by default)."""
    m = max(up, down)
    return firwin(2 * 10 * m + 1, 1.0 / m, window=("kaiser", 5.0)) * up


def resample(x, up, down, h=None):
    return resample_poly(x, up, down, window=design(up, down) if h is None else h)


def to_cu8(y, k):
    """complex -> interleaved uint8 I,Q with offset 127 (what mode-s.c expects).
    Returns (bytes array, number of clipped rail samples)."""
    iq = np.empty(2 * len(y), np.float32)
    iq[0::2] = y.real
    iq[1::2] = y.imag
    v = np.round(iq * k) + 127.0
    clipped = int(((v < 0) | (v > 255)).sum())
    return np.clip(v, 0, 255).astype(np.uint8), clipped
