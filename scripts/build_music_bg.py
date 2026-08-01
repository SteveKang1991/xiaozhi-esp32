#!/usr/bin/env python3
"""
Convert every board-specific music background PNG in main/assets/ into an
LVGL CBin-format .bin image, dropping the resulting .bin files into
<output-dir>. Each (png, bin-name) pair is hard-coded below so the same
script always produces the full set of backgrounds, regardless of which
board is currently being built.

Each output bin has the layout cbin_img_dsc_create() expects:
    [ lv_image_dsc_t struct ]   <-- 28 bytes on 32-bit ESP32 (sizeof the struct)
    [ raw RGB565 pixel data ]   <-- pointed to by struct.data (relative offset)

Because cbin_img_dsc_create does `img_dsc->data += bin_addr`, the .data
field of the struct must be the offset (from the start of the bin) of the
pixel data, not an absolute address. The C side handles the relocation.

PNG -> bin mapping (fixed; one bin per board):
    main/assets/musicbg-480x816.png   -> musicbg-480x816.bin   (480 x 816)
    main/assets/musicbg-720x1232.png  -> musicbg-720x1232.bin  (720 x 1232)

Both bins are always produced in a single invocation; the script does
not branch on BOARD_TYPE. The board that needs a particular bin will
look it up directly on the SD card at runtime, so producing the wrong
one for the current build is harmless.

The C-side caller (display code) opens /sdcard/Music/musicbg-<w>x<h>.bin
at runtime, so the developer copies whichever bin(s) apply to the SD
card under /sdcard/Music/.

Usage:
  python build_music_bg.py --assets-source-dir <main/assets> \
      --output-dir <build>
"""

import argparse
import os
import struct
import subprocess
import sys

# Make LVGLImage importable when run from anywhere.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "Image_Converter"))


def _ensure_python_deps():
    """Install pip deps that LVGLImage needs, on demand.

    The script may run from the ESP-IDF Python environment, which is a clean
    venv without pypng/lz4 pre-installed. Rather than asking every developer
    to manually `pip install` them, we try to import first and fall back to
    `python -m pip install` if anything is missing. Failures bubble up so
    CMake gets a clean non-zero exit and prints a useful traceback.
    """
    # Map: import name -> pip distribution name. They differ for some packages.
    needed = [
        ("png",   "pypng"),
        ("lz4",   "lz4"),
    ]
    missing_pkgs = []
    for mod_name, pkg_name in needed:
        try:
            __import__(mod_name)
        except ImportError:
            missing_pkgs.append(pkg_name)
    if not missing_pkgs:
        return

    print(f"[build_music_bg] missing python deps: {', '.join(missing_pkgs)}; "
          f"installing via pip...", file=sys.stderr)

    # Try install strategies in order of preference:
    #   1. `--user`             — works on most dev machines
    #   2. plain install        — works inside a venv (e.g. ESP-IDF Python env)
    #   3. `--user --break-system-packages` — PEP 668 distros
    strategies = [
        [sys.executable, "-m", "pip", "install", "--user"],
        [sys.executable, "-m", "pip", "install"],
        [sys.executable, "-m", "pip", "install", "--user",
         "--break-system-packages"],
    ]
    last_err = None
    for base in strategies:
        cmd = base + missing_pkgs
        print(f"[build_music_bg] running: {' '.join(cmd)}", file=sys.stderr)
        try:
            subprocess.check_call(cmd)
        except subprocess.CalledProcessError as e:
            last_err = e
            continue
        # Verify after install. The site-packages dir that pip just wrote to
        # may not be on sys.path yet (pip modifies site-packages, but the
        # current interpreter's sys.path was frozen at startup), so add it
        # explicitly before re-checking.
        try:
            import site  # noqa: F401
            user_site = site.getusersitepackages()
            if user_site and user_site not in sys.path:
                sys.path.insert(0, user_site)
        except Exception:
            pass
        all_ok = True
        for mod_name, _pkg_name in needed:
            try:
                __import__(mod_name)
            except ImportError:
                all_ok = False
                break
        if all_ok:
            return
        last_err = RuntimeError("pip install exited 0 but module still missing")

    raise RuntimeError(
        f"Failed to install Python dependencies {missing_pkgs}. "
        f"Last error: {last_err}. "
        f"Try manually: {sys.executable} -m pip install {' '.join(missing_pkgs)}"
    )


_ensure_python_deps()

from LVGLImage import LVGLImage, ColorFormat, CompressMethod  # noqa: E402


# Hard-coded PNG -> bin mapping (independent of which board is being built).
# Width is the screen width (no scaling); height matches the corresponding
# board's ROI / display height so the .bin lands flush in the
# music_cover_container without leaving an under-filled stripe.
_PNG_BIN_TABLE = [
    # (png filename,        width, height, output bin name)
    ("musicbg-480x816.png",  480,  816,  "musicbg-480x816.bin"),
    ("musicbg-720x1232.png", 720,  1232, "musicbg-720x1232.bin"),
]

# Must match sizeof(lv_image_dsc_t) on the target (32-bit ESP32). Both
# members after `header` are 4 bytes each (data_size, data, reserved,
# reserved_2), so the struct header is 12 + 16 = 28 bytes on the device.
# We hard-code 28 instead of relying on a C header so the Python pipeline
# stays self-contained.
LV_IMG_DSC_HEADER_SIZE = 28


def convert_png_to_cbin_bin(png_path: str, bin_path: str,
                            width: int, height: int) -> int:
    """Convert a PNG to an LVGL CBin-format .bin.

    The output file is exactly what cbin_img_dsc_create() expects: a
    `lv_image_dsc_t` struct (with .data = relative offset to pixel data)
    followed by raw RGB565 pixel bytes.

    Returns the bin file size in bytes.
    """
    # The PNG is already at the target width/height (we don't resize).
    # Loading via LVGLImage converts the PNG to RGB565 raw bytes.
    img = LVGLImage().from_png(png_path, cf=ColorFormat.RGB565)

    if img.w != width or img.h != height:
        # Sanity: warn loudly so a wrong-dimension source PNG doesn't
        # silently produce a mismatched image.
        print(f"[build_music_bg] WARNING: source PNG {png_path} is "
              f"{img.w}x{img.h}, expected {width}x{height}; bin will still "
              f"be emitted at the actual source dimensions.")

    data_size = len(img.data)
    if data_size == 0:
        raise RuntimeError(f"LVGLImage produced empty pixel data for {png_path}")

    # Write: [struct (28 bytes)] [pixel data]
    # The struct.data field holds the offset (from bin start) of the pixel
    # data, which cbin_img_dsc_create() will relocate by adding bin_addr.
    with open(bin_path, "wb") as f:
        # ---- lv_image_header_t (12 bytes) ----
        # Layout (matches LVGL 9.x `lv_image_header_t`):
        #   uint8_t  magic;       // 1
        #   uint8_t  cf;          // 1
        #   uint16_t flags;       // 2
        #   uint16_t w;           // 2
        #   uint16_t h;           // 2
        #   uint16_t stride;      // 2
        #   uint16_t reserved;    // 2
        # Total = 12 bytes (no trailing padding; all-natural alignment).
        flags = 0
        f.write(struct.pack("<BBHHHHH",
                            0x19,                         # magic = LV_IMAGE_HEADER_MAGIC
                            img.cf.value & 0xFF,          # color format (RGB565 = 0x12)
                            flags & 0xFFFF,               # flags (uint16_t LE)
                            img.w & 0xFFFF, img.h & 0xFFFF,
                            img.stride & 0xFFFF,
                            0))                           # reserved
        # ---- lv_image_dsc_t trailing fields (16 bytes) ----
        f.write(struct.pack("<I", data_size))                          # data_size
        f.write(struct.pack("<I", LV_IMG_DSC_HEADER_SIZE))            # data (relocated by +bin_addr)
        f.write(struct.pack("<I", 0))                                  # reserved
        f.write(struct.pack("<I", 0))                                  # reserved_2
        # ---- raw RGB565 pixel bytes ----
        f.write(img.data)

    return LV_IMG_DSC_HEADER_SIZE + data_size


def build_all(assets_source_dir: str, output_dir: str) -> int:
    """Convert every entry in _PNG_BIN_TABLE.

    Returns 0 on success, non-zero on failure. A missing source PNG for
    any board is an error (the developer is expected to ship all of
    them together).
    """
    os.makedirs(output_dir, exist_ok=True)
    produced = []
    for png_name, w, h, bin_name in _PNG_BIN_TABLE:
        png_path = os.path.join(assets_source_dir, png_name)
        if not os.path.exists(png_path):
            print(f"[build_music_bg] ERROR: source PNG missing: {png_path}",
                  file=sys.stderr)
            return 1
        bin_path = os.path.join(output_dir, bin_name)
        size = convert_png_to_cbin_bin(png_path, bin_path, w, h)
        produced.append((bin_path, size))
        print(f"[build_music_bg] OK: {bin_path} ({size} bytes)")
    print(f"[build_music_bg] generated {len(produced)} bin file(s) in "
          f"{output_dir}; copy the one(s) matching your board to the SD "
          f"card under /sdcard/Music/.")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Build music_bg_<board>.bin images for every board.")
    parser.add_argument("--assets-source-dir", required=True,
                        help="Directory containing the musicbg-*.png files "
                             "(usually main/assets).")
    parser.add_argument("--output-dir", required=True,
                        help="Directory to drop the .bin files into. The "
                             "developer copies whichever bin matches the "
                             "current board to the SD card at "
                             "/sdcard/Music/musicbg-<w>x<h>.bin.")
    args = parser.parse_args()
    return build_all(args.assets_source_dir, args.output_dir)


if __name__ == "__main__":
    sys.exit(main())
