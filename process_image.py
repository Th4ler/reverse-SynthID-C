import os
import sys
import argparse
import subprocess
import numpy as np
from PIL import Image

def parse_args():
    parser = argparse.ArgumentParser(
        description="High-fidelity Google SynthID Watermark Bypass Utility (Simple & Smart Methods)."
    )
    parser.add_argument(
        "input_image",
        type=str,
        help="Path to the input watermarked image."
    )
    parser.add_argument(
        "--codebook",
        type=str,
        default=os.path.join("lib", "codebook-robust.bin"),
        help="Path to the robust codebook binary file (default: lib/codebook-robust.bin)."
    )
    parser.add_argument(
        "--strength",
        type=str,
        default="aggressive",
        choices=["gentle", "moderate", "aggressive", "maximum"],
        help="Watermark bypass cancellation strength (default: aggressive)."
    )
    parser.add_argument(
        "--exe",
        type=str,
        default=None,
        help="Path to the reverse_synthid binary. If not specified, auto-resolves based on OS."
    )
    return parser.parse_args()

def main():
    args = parse_args()
    
    input_path = args.input_image
    if not os.path.exists(input_path):
        print(f"Error: Input image not found at '{input_path}'", file=sys.stderr)
        sys.exit(1)

    codebook_path = args.codebook
    if not os.path.exists(codebook_path):
        print(f"Error: Codebook file not found at '{codebook_path}'", file=sys.stderr)
        sys.exit(1)

    # Auto-resolve executable path based on OS if not provided
    exe_path = args.exe
    if not exe_path:
        is_windows = sys.platform.startswith("win")
        exe_name = "reverse_synthid.exe" if is_windows else "reverse_synthid"
        # Check current directory first, then fallback to system PATH
        local_exe = os.path.join(".", exe_name)
        if os.path.exists(local_exe):
            exe_path = local_exe
        else:
            exe_path = exe_name

    # Derive output paths dynamically
    base, ext = os.path.splitext(input_path)
    output_simple_path = f"{base}_bypassed_simple{ext}"
    output_smart_path = f"{base}_bypassed_smart{ext}"

    # Load original image
    print(f"Loading original image from {input_path}...")
    orig_img = Image.open(input_path).convert("RGB")
    orig_w, orig_h = orig_img.size
    print(f"Original size: {orig_w}x{orig_h}")

    # Step 1: Resize to 512x512 for bypass
    print("Resizing image to 512x512 for watermark extraction...")
    temp_512_in = "temp_512_in.png"
    temp_512_out = "temp_512_out.png"
    
    img_512 = orig_img.resize((512, 512), Image.Resampling.LANCZOS)
    img_512.save(temp_512_in)

    # Step 2: Run C bypass tool
    print(f"Running watermark bypass via '{exe_path}' ({args.strength} strength)...")
    cmd = [exe_path, "bypass", temp_512_in, temp_512_out, codebook_path, args.strength]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        if result.stdout:
            print("STDOUT:", result.stdout.strip())
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"Error: Failed to execute reverse_synthid binary: {e}", file=sys.stderr)
        # Cleanup if temp files were created
        for temp_file in [temp_512_in, temp_512_out]:
            if os.path.exists(temp_file):
                os.remove(temp_file)
        sys.exit(1)

    if not os.path.exists(temp_512_out):
        print("Error: Bypassed 512x512 image was not generated.", file=sys.stderr)
        if os.path.exists(temp_512_in):
            os.remove(temp_512_in)
        sys.exit(1)

    # Step 3: Simple method (resize bypassed 512x512 back to original size)
    print("Generating simple bypassed image (direct upscale)...")
    bypassed_512 = Image.open(temp_512_out).convert("RGB")
    img_simple = bypassed_512.resize((orig_w, orig_h), Image.Resampling.LANCZOS)
    img_simple.save(output_simple_path)
    print(f"Simple bypassed image saved to '{output_simple_path}'")

    # Step 4: Smart method (extract and upscale only the watermark noise)
    print("Generating smart bypassed image (extracting and scaling watermark noise)...")
    arr_orig_512 = np.array(img_512, dtype=np.float32)
    arr_byp_512 = np.array(bypassed_512, dtype=np.float32)
    
    # Noise = Original - Bypassed (this is the watermark signal that was subtracted)
    noise_512 = arr_orig_512 - arr_byp_512
    print(f"Watermark noise extracted (maximum spatial difference at 512x512: {np.max(np.abs(noise_512)):.2f})")

    # Resize noise back to original dimensions channel by channel
    noise_orig = np.zeros((orig_h, orig_w, 3), dtype=np.float32)
    for ch in range(3):
        ch_noise_img = Image.fromarray(noise_512[:, :, ch], mode='F')
        # Lanczos preserves high-frequency watermark patterns with extreme fidelity
        ch_noise_resized = ch_noise_img.resize((orig_w, orig_h), Image.Resampling.LANCZOS)
        noise_orig[:, :, ch] = np.array(ch_noise_resized)

    # Subtract the scaled noise from the original high-resolution image
    arr_orig = np.array(orig_img, dtype=np.float32)
    arr_smart = arr_orig - noise_orig
    
    # Clamp to valid RGB range and convert to uint8
    arr_smart = np.clip(arr_smart, 0.0, 255.0)
    img_smart = Image.fromarray(arr_smart.astype(np.uint8), mode="RGB")
    img_smart.save(output_smart_path)
    print(f"Smart bypassed image saved to '{output_smart_path}'")

    # Clean up temp files
    for temp_file in [temp_512_in, temp_512_out]:
        try:
            if os.path.exists(temp_file):
                os.remove(temp_file)
        except OSError:
            pass

    # Step 5: Verify results on both outputs
    print("\n--- Verifying watermark on Simple Bypassed Image ---")
    cmd_verify_simple = [exe_path, "detect", output_simple_path, codebook_path]
    res_simple = subprocess.run(cmd_verify_simple, capture_output=True, text=True)
    print(res_simple.stdout.strip())

    print("\n--- Verifying watermark on Smart Bypassed Image ---")
    cmd_verify_smart = [exe_path, "detect", output_smart_path, codebook_path]
    res_smart = subprocess.run(cmd_verify_smart, capture_output=True, text=True)
    print(res_smart.stdout.strip())

if __name__ == "__main__":
    main()
