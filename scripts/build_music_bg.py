#!/usr/bin/env python3
"""
Pick the right music background PNG for the current board and convert it
into an LVGL CBin .bin image that the firmware can memory-map directly
from the assets partition and feed to LvglCBinImage / cbin_img_dsc_create.

Board -> PNG mapping (width == screen width; height matches the display's
native ROI):
  - BOARD_TYPE_FANFUTURE_P4_HOLO_ST7701_WiFI6_LCD_5B   -> musicbg-480x816.png
  - BOARD_TYPE_FANFUTURE_P4_HOLO_ILI9881_WiFI6_LCD_55B -> musicbg-720x1232.png

The on-disk layout we produce matches what cbin_img_dsc_create expects:
    [ lv_image_dsc_t struct ]   <-- 28 bytes on 32-bit ESP32 (sizeof the struct)
    [ raw RGB565 pixel data ]   <-- pointed to by struct.data (relative offset)

Because cbin_img_dsc_create does `img_dsc->data += bin_addr`, the .data
field of the struct must be the offset (from the start of the bin) of the
pixel data, not an absolute address. The C side handles the relocation.

The output filename is always music_bg.bin (a fixed name) so the C side
can do Assets::GetAssetData("music_bg.bin", ...).

Usage:
  python build_music_bg.py --sdkconfig <path> \
      --assets-source-dir <main/assets> \
      --output-dir <build/assets>
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


# Map sdkconfig symbol -> (png filename, output width, output height).
# Width is the screen width (user's request: no scaling); height matches
# the corresponding board's ROI / display height so the .bin lands flush
# in the music_cover_container without leaving an under-filled stripe.
_BOARD_MAP = {
    "CONFIG_BOARD_TYPE_FANFUTURE_P4_HOLO_ST7701_WiFI6_LCD_5B": (
        "musicbg-480x816.png", 480, 816),
    "CONFIG_BOARD_TYPE_FANFUTURE_P4_HOLO_ILI9881_WiFI6_LCD_55B": (
        "musicbg-720x1232.png", 720, 1232),
}

OUTPUT_BIN_NAME = "music_bg.bin"

# Must match sizeof(lv_image_dsc_t) on the target (32-bit ESP32). Both
# members after `header` are 4 bytes each (data_size, data, reserved,
# reserved_2), so the struct header is 12 + 16 = 28 bytes on the device.
# We hard-code 28 instead of relying on a C header so the Python pipeline
# stays self-contained.
LV_IMG_DSC_HEADER_SIZE = 28


def find_board(sdkconfig_path: str):
    """Return the first matching (png, w, h) tuple for this sdkconfig."""
    if not os.path.exists(sdkconfig_path):
        return None
    # sdkconfig is normally UTF-8, but tools like PowerShell may write a
    # copy in UTF-16 (with BOM) when redirecting. Try UTF-16 first if a
    # BOM is present, otherwise fall back to UTF-8 with replacement.
    # After decoding, strip any leftover BOM/ZWNBSP that some codecs keep.
    raw = open(sdkconfig_path, "rb").read()
    if raw.startswith(b"\xff\xfe"):
        text = raw.decode("utf-16-le", errors="ignore")
    elif raw.startswith(b"\xfe\xff"):
        text = raw.decode("utf-16-be", errors="ignore")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw[3:].decode("utf-8", errors="ignore")
    else:
        text = raw.decode("utf-8", errors="ignore")
    text = text.lstrip("\ufeff")

    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            continue
        key = line.split("=", 1)[0].strip()
        if key in _BOARD_MAP:
            val = line.split("=", 1)[1].strip()
            if val in ("y", "y\r"):
                return _BOARD_MAP[key]
    return None


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
        print(f"[build_music_bg] WARNING: source PNG is {img.w}x{img.h}, "
              f"expected {width}x{height}; bin will still be emitted at "
              f"the actual source dimensions.")

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


def main():
    parser = argparse.ArgumentParser(
        description="Build the music_bg.bin image for the current board.")
    parser.add_argument("--sdkconfig", required=True,
                        help="Path to sdkconfig file.")
    parser.add_argument("--assets-source-dir", required=True,
                        help="Directory containing the musicbg-*.png files "
                             "(usually main/assets).")
    parser.add_argument("--output-dir", required=True,
                        help="Directory to drop music_bg.bin into "
                             "(consumed by build_default_assets.py).")
    args = parser.parse_args()

    board = find_board(args.sdkconfig)
    if board is None:
        print("[build_music_bg] current board has no music_bg mapping, skip.")
        return 0

    png_name, w, h = board
    png_path = os.path.join(args.assets_source_dir, png_name)
    if not os.path.exists(png_path):
        print(f"[build_music_bg] ERROR: source PNG missing: {png_path}",
              file=sys.stderr)
        return 1

    os.makedirs(args.output_dir, exist_ok=True)
    bin_path = os.path.join(args.output_dir, OUTPUT_BIN_NAME)
    size = convert_png_to_cbin_bin(png_path, bin_path, w, h)
    print(f"[build_music_bg] OK: {bin_path} ({size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())