#!/usr/bin/env python3
"""
convert_codebook.py

Converts a reverse-SynthID v2.0 .pkl codebook to the multi-resolution .bin
format expected by reverse_synthid (the C port).

Produces one profile per scale listed in the pkl's 'scales_used' field
(typically 256, 512, 1024).  The reference_noise array is resized to each
scale and FFT'd independently so carrier bin positions are correct at every
resolution.

Usage:
    python convert_codebook.py <input.pkl> <output.bin>
"""

import sys, struct, pickle, types
import numpy as np
from PIL import Image as PILImage   # only needed for resize; falls back to numpy

# ── NumPy 2.x → 1.x shim ────────────────────────────────────────────────────
def _install_numpy_shims():
    import numpy.core as _npc
    def _proxy(name, src):
        m = types.ModuleType(name); m.__dict__.update(src.__dict__)
        sys.modules[name] = m
    _proxy("numpy._core", _npc)
    for a in dir(_npc):
        o = getattr(_npc, a)
        if isinstance(o, types.ModuleType): _proxy(f"numpy._core.{a}", o)
    for s in ("multiarray","numeric","fromnumeric","umath"):
        try:
            import importlib; _proxy(f"numpy._core.{s}",
                                     importlib.import_module(f"numpy.core.{s}"))
        except ImportError: pass

if not hasattr(np, "_core"):
    _install_numpy_shims()
# ─────────────────────────────────────────────────────────────────────────────

MAGIC_MULTI  = b"SCBM"
MAX_CARRIERS = 512
TOP_CARRIERS = 128
NOISE_RADIUS = 3

# ── Helpers ──────────────────────────────────────────────────────────────────

def box_blur(arr, radius):
    """Separable box blur matching the C make_noise radius."""
    from scipy.ndimage import uniform_filter
    return uniform_filter(arr.astype(np.float64), size=2*radius+1, mode='nearest')

def make_noise(plane, radius=NOISE_RADIUS):
    try:
        return plane.astype(np.float64) - box_blur(plane, radius)
    except ImportError:
        # fallback: no scipy — use numpy convolve approximation
        from numpy import pad
        d = 2*radius+1
        k = np.ones((d,d), dtype=np.float64) / (d*d)
        blurred = np.array([[
            plane[max(0,r-radius):r+radius+1, max(0,c-radius):c+radius+1].mean()
            for c in range(plane.shape[1])] for r in range(plane.shape[0])])
        return plane.astype(np.float64) - blurred

def resize_noise(noise_hwc, new_size):
    """Resize (H,W,3) noise array to (new_size, new_size, 3)."""
    try:
        from PIL import Image as Im
        out = np.zeros((new_size, new_size, 3), dtype=np.float64)
        for ch in range(3):
            plane = noise_hwc[:,:,ch]
            # PIL needs uint8 for resize; scale to [0,255], resize, scale back
            mn, mx = plane.min(), plane.max()
            if mx == mn:
                out[:,:,ch] = 0; continue
            scaled = ((plane - mn) / (mx - mn) * 255).astype(np.uint8)
            resized = np.array(Im.fromarray(scaled).resize(
                (new_size, new_size), Im.BILINEAR), dtype=np.float64)
            out[:,:,ch] = resized / 255.0 * (mx - mn) + mn
        return out
    except ImportError:
        # Nearest-neighbour fallback
        H, W, _ = noise_hwc.shape
        rs = (np.arange(new_size) * H / new_size).astype(int)
        cs = (np.arange(new_size) * W / new_size).astype(int)
        return noise_hwc[np.ix_(rs, cs)]

def compute_fft_channels(noise_hwc, size):
    """FFT-shift each channel of a (size,size,3) noise array."""
    fft_channels = []
    for ch in range(3):
        plane = noise_hwc[:,:,ch]
        f = np.fft.fftshift(np.fft.fft2(plane))
        fft_channels.append(f.astype(np.complex64))
    return fft_channels   # list of 3 x (size,size) complex arrays

def select_carriers(fft_channels, size, n_top=TOP_CARRIERS):
    centre = size // 2
    green = np.abs(fft_channels[1])
    masked = green.copy()
    masked[centre-4:centre+4, centre-4:centre+4] = 0

    flat = masked.flatten()
    top_idx = np.argpartition(flat, -n_top)[-n_top:]
    top_idx = top_idx[np.argsort(flat[top_idx])[::-1]]

    rows = (top_idx // size).tolist()
    cols = (top_idx %  size).tolist()

    ref_phases, ref_mags = [[], [], []], [[], [], []]
    for ch in range(3):
        for r, c in zip(rows, cols):
            z = fft_channels[ch][r, c]
            ref_phases[ch].append(float(np.angle(z)))
            ref_mags[ch].append(float(np.abs(z)))
    return rows, cols, ref_phases, ref_mags

# ── Write helpers ─────────────────────────────────────────────────────────────

def write_profile(f, size, fft_channels, carrier_rs, carrier_cs,
                  ref_phases, ref_mags,
                  threshold_corr, threshold_phase, corr_mean, corr_std):
    n = len(carrier_rs)
    f.write(struct.pack('<ii', size, size))
    for ch in range(3):
        arr = fft_channels[ch].astype(np.complex64).flatten()
        interleaved = np.empty(size*size*2, dtype=np.float32)
        interleaved[0::2] = arr.real
        interleaved[1::2] = arr.imag
        f.write(interleaved.tobytes())
    f.write(struct.pack('<i', n))
    f.write(np.array(carrier_rs, dtype=np.int32).tobytes())
    f.write(np.array(carrier_cs, dtype=np.int32).tobytes())
    for ch in range(3):
        f.write(np.array(ref_phases[ch], dtype=np.float32).tobytes())
    for ch in range(3):
        f.write(np.array(ref_mags[ch],   dtype=np.float32).tobytes())
    f.write(struct.pack('<ffff',
                        float(threshold_corr), float(threshold_phase),
                        float(corr_mean),      float(corr_std)))

# ── Main conversion ───────────────────────────────────────────────────────────

def convert(pkl_path, bin_path):
    print(f"Loading {pkl_path} ...")
    with open(pkl_path, "rb") as f:
        cb = pickle.load(f)

    print(f"  Version  : {cb.get('version','?')}")
    print(f"  Source   : {cb.get('source','?')}")
    print(f"  Extractor: {cb.get('extractor','?')}")
    print(f"  Images   : {cb.get('n_images_analyzed','?')}")

    native_size = int(cb['image_size'])         # 512
    scales      = sorted(cb.get('scales_used', [native_size]))   # [256,512,1024]
    print(f"  Native   : {native_size}x{native_size}")
    print(f"  Scales   : {scales}")

    ref_noise = cb['reference_noise']           # (512,512,3) float64

    # Compute noise at native size once, then resize for other scales
    noise_native = np.zeros_like(ref_noise, dtype=np.float64)
    for ch in range(3):
        noise_native[:,:,ch] = make_noise(ref_noise[:,:,ch])

    # Thresholds from pkl
    det_thresh  = float(cb.get('detection_threshold', 0.010))
    corr_mean   = float(cb.get('correlation_mean',    0.0))
    corr_std    = float(cb.get('correlation_std',     0.003))
    corr_thresh = max(det_thresh, corr_mean + 2.0 * corr_std)
    phase_thresh = 0.45

    # pkl carriers — convert freq offsets to absolute indices per scale
    pkl_carriers   = cb.get('carriers', [])
    pkl_known      = cb.get('known_carriers', [])

    print(f"\nWriting {bin_path} with {len(scales)} profile(s) ...")

    with open(bin_path, "wb") as out:
        out.write(MAGIC_MULTI)
        out.write(struct.pack('<i', len(scales)))

        for scale in scales:
            print(f"  Scale {scale}x{scale} ...", end=" ", flush=True)
            centre = scale // 2

            # Resize noise to this scale
            if scale == native_size:
                noise_at_scale = noise_native
            else:
                noise_at_scale = resize_noise(noise_native, scale)

            fft_ch = compute_fft_channels(noise_at_scale, scale)

            # Build carrier list for this scale.
            # Carrier freq offsets in the pkl are at whatever scale they were
            # detected; the 'scales' field on each carrier says which sizes
            # they came from.  Scale the offset proportionally.
            carrier_rs, carrier_cs = [], []
            seen = set()

            def add_carrier(fr, fc):
                abs_r = centre + fr
                abs_c = centre + fc
                if not (0 <= abs_r < scale and 0 <= abs_c < scale): return
                key = (abs_r, abs_c)
                if key in seen: return
                seen.add(key)
                carrier_rs.append(abs_r)
                carrier_cs.append(abs_c)

            for entry in pkl_carriers:
                freq = entry['frequency']
                src_scales = entry.get('scales', [native_size])
                # Use the smallest source scale for the ratio (most reliable)
                src_scale = min(src_scales) if src_scales else native_size
                ratio = scale / src_scale
                fr = round(int(freq[0]) * ratio)
                fc = round(int(freq[1]) * ratio)
                add_carrier(fr, fc)

            for freq in pkl_known:
                # known_carriers are always expressed at native_size
                ratio = scale / native_size
                fr = round(int(freq[0]) * ratio)
                fc = round(int(freq[1]) * ratio)
                add_carrier(fr, fc)

            # If we ended up with very few carriers, augment from FFT magnitude
            if len(carrier_rs) < 16:
                extra_r, extra_c, _, _ = select_carriers(fft_ch, scale,
                                                          n_top=TOP_CARRIERS)
                for r, c in zip(extra_r, extra_c):
                    key = (r, c)
                    if key not in seen:
                        seen.add(key); carrier_rs.append(r); carrier_cs.append(c)

            # Trim to MAX_CARRIERS
            carrier_rs = carrier_rs[:MAX_CARRIERS]
            carrier_cs = carrier_cs[:MAX_CARRIERS]

            # Derive ref phases/mags from the FFT at this scale
            ref_phases, ref_mags = [[], [], []], [[], [], []]
            for ch in range(3):
                for r, c in zip(carrier_rs, carrier_cs):
                    z = fft_ch[ch][r, c]
                    ref_phases[ch].append(float(np.angle(z)))
                    ref_mags[ch].append(float(np.abs(z)))

            write_profile(out, scale, fft_ch,
                          carrier_rs, carrier_cs, ref_phases, ref_mags,
                          corr_thresh, phase_thresh, corr_mean, corr_std)
            print(f"{len(carrier_rs)} carriers")

    print(f"\nDone. {bin_path}")
    print(f"  corr_threshold : {corr_thresh:.6f}")
    print(f"  phase_threshold: {phase_thresh}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.pkl> <output.bin>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
