# reverse_synthid - C port

C translation of [aloshdenny/reverse-SynthID](https://github.com/aloshdenny/reverse-SynthID).

All logic lives in a **single file** (`reverse_synthid.c`) with no runtime
dependencies beyond `libc` and `libm`.  The only build-time header you need
to supply is the well-known **stb_image** single-header library (see below).

The initial C translation was made using Claude. (Free version, because I'm cheap.)

---

## What it does

| Mode      | What happens |
|-----------|-------------|
| `extract` | Averages noise residuals from a directory of reference images (solid-colour AI outputs) to build a spread-spectrum codebook |
| `detect`  | Loads a codebook and reports whether a given image carries a SynthID watermark, along with correlation / phase-match / structure-ratio scores |
| `bypass`  | Surgically removes the watermark in the frequency domain using codebook-guided subtraction at carrier bins |

All three modes are faithful C translations of the Python originals.

---

## Dependencies

| Dependency | How to get it |
|------------|--------------|
| `stb_image.h` | `curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image.h` |
| `stb_image_write.h` | `curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h` |

Place both files next to `reverse_synthid.c`.

The 2-D FFT is a self-contained Cooley–Tukey radix-2 implementation included
directly in `reverse_synthid.c` — no FFTW or KissFFT required.

---

## Build

```sh
# Fetch stb headers (one-time)
curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
curl -O https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h

# Build
make
# or manually:
gcc -O2 -o reverse_synthid reverse_synthid.c -lm
```

Works on Linux, macOS, and Windows (MinGW/MSVC with minor path adjustments).

---

## Usage

You need a codebook. The script `helpers/convert_codebook.pl` converts the original Python pickle files to binary files for this program.
A sample binary codebook is in the `lib/` directory.

### 1. Extract a codebook from reference images

```sh
reverse_synthid extract /path/to/reference/images/ codebook.bin
```

Reference images should be solid white/black images produced by Gemini so
that every non-zero pixel is watermark, not content.

### 2. Detect a watermark

```sh
reverse_synthid detect suspect_image.png codebook.bin
```

Example output:

```
Detection Results:
  Watermarked    : YES
  Confidence     : 0.9971
  Correlation    : 0.5355
  Phase Match    : 0.9571
  Structure Ratio: 1.2753
```

Exit code `0` = watermarked, `2` = not watermarked, `1` = error.

### 3. Remove the watermark

```sh
reverse_synthid bypass input.png output.png codebook.bin [gentle|moderate|aggressive|maximum]
```

Strength levels match the Python original:

| Level        | Removal fraction | Notes |
|--------------|-----------------|-------|
| `gentle`     | 25 %            | ~45 dB PSNR, barely visible change |
| `moderate`   | 50 %            | Good balance |
| `aggressive` | 75 %            | **Default** — matches Python V3 |
| `maximum`    | 95 %            | May introduce slight ringing |

All levels hard-cap subtraction at 30 % of the bin's own energy, preventing
content damage (same rule as the Python version).

---

## Algorithm summary

```
DETECT
  1. Extract noise residual: noise = image – box_blur(image, r=3)
  2. 2-D FFT → FFT-shift to centre DC
  3. At each codebook carrier bin, measure:
       - normalised cross-correlation with reference magnitude/phase
       - phase deviation from reference
  4. Per-channel weights: G=1.0, R=0.85, B=0.70
  5. Thresholds: correlation > 0.179, phase_match > 0.50,
                 0.8 < structure_ratio < 1.8

BYPASS
  1. FFT each colour channel
  2. At carrier bins, subtract:
       delta = codebook_mag × frac × channel_weight
       delta = min(delta, 0.30 × bin_mag)   # content cap
  3. Inverse FFT → clamp [0, 255]

EXTRACT
  1. For each reference image: noise → FFT → accumulate
  2. Divide by image count → average FFT
  3. Select top-128 magnitude bins (excluding DC) as carriers
```

---

## Notes on image size

SynthID embeds carriers at **resolution-dependent absolute frequency bins**.
The codebook stores the size it was built at.  If your target images differ
in resolution, rebuild the codebook from same-resolution references, or
add a resize step before detection/bypass.

---

## License

Research and educational use only - same terms as the upstream Python project.
SynthID is proprietary technology owned by Google DeepMind.
