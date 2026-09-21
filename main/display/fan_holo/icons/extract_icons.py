import re
import io
from pathlib import Path
from PIL import Image

def extract_jpeg(h_path: Path, out_jpg: Path) -> Image.Image:
    text = h_path.read_text(encoding="utf-8", errors="ignore")
    nums = re.findall(r"0x([0-9A-Fa-f]{2})", text)
    data = bytes(int(x, 16) for x in nums)
    out_jpg.write_bytes(data)
    im = Image.open(io.BytesIO(data)).convert("RGBA")
    print(h_path, "->", im.size, "bytes", len(data))
    return im


def to_lvgl_c(im: Image.Image, var_name: str, out_c: Path, out_h: Path) -> None:
    """Write LVGL v9 RGB565 + alpha (CF_RGB565A8) image descriptor."""
    im = im.convert("RGBA")
    w, h = im.size
    pixels = list(im.getdata())
    rgb565 = bytearray()
    alpha = bytearray()
    for r, g, b, a in pixels:
        # LVGL RGB565 typically stored little-endian
        c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        rgb565.append(c & 0xFF)
        rgb565.append((c >> 8) & 0xFF)
        alpha.append(a)

    data = bytes(rgb565) + bytes(alpha)
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append(", ".join(f"0x{b:02X}" for b in chunk) + ",")

    c_body = f"""#include "lvgl.h"
#include "{out_h.name}"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

const LV_ATTRIBUTE_MEM_ALIGN uint8_t {var_name}_map[] = {{
{chr(10).join(lines)}
}};

const lv_image_dsc_t {var_name} = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565A8,
        .flags = 0,
        .w = {w},
        .h = {h},
        .stride = {w * 2},
    }},
    .data_size = sizeof({var_name}_map),
    .data = {var_name}_map,
}};
"""
    h_body = f"""#pragma once
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {{
#endif

extern const lv_image_dsc_t {var_name};

#ifdef __cplusplus
}}
#endif
"""
    out_c.write_text(c_body, encoding="utf-8")
    out_h.write_text(h_body, encoding="utf-8")
    print("wrote", out_c, out_h, w, h)


def main() -> None:
    out = Path(__file__).resolve().parent
    base = Path(r"C:\Users\Administrator\Desktop\ESP32-S3-AmourK08-4Keys\K08-4Keys-SmallDesk")
    temp = extract_jpeg(base / "src/img/temperature.h", out / "temperature.jpg")
    hum = extract_jpeg(base / "src/img/humidity.h", out / "humidity.jpg")

    # Match desk-clock metric icons at 24x24; precip derived from light-rain art.
    temp24 = temp.resize((24, 24), Image.LANCZOS) if temp.size != (24, 24) else temp
    hum24 = hum.resize((24, 24), Image.LANCZOS) if hum.size != (24, 24) else hum

    # Reference projects have no dedicated precip metric icon (only sky codes).
    # Draw a 24x24 raindrop matching the desk-clock white-outline style.
    precip24 = Image.new("RGBA", (24, 24), (0, 0, 0, 0))
    px = precip24.load()
    # Tear / drop silhouette (white outline + cyan fill).
    drop = [
        "............",
        "......WW....",
        ".....WCCW...",
        "....WCCCCW..",
        "...WCCCCCCW.",
        "..WCCCCCCCCW",
        ".WCCCCCCCCCW",
        ".WCCCCCCCCCW",
        ".WCCCCCCCCCW",
        "..WCCCCCCCCW",
        "...WCCCCCCW.",
        "....WCCCCW..",
        ".....WCCW...",
        "......WW....",
        "............",
    ]
    ox, oy = 6, 4
    for y, row in enumerate(drop):
        for x, ch in enumerate(row):
            if ch == "W":
                px[ox + x, oy + y] = (255, 255, 255, 255)
            elif ch == "C":
                px[ox + x, oy + y] = (80, 200, 255, 255)

    # Make near-black pixels transparent so icons sit cleanly on LED card.
    def punch_black(im: Image.Image, thr: int = 28) -> Image.Image:
        px = im.load()
        for y in range(im.height):
            for x in range(im.width):
                r, g, b, a = px[x, y]
                if a > 0 and r < thr and g < thr and b < thr:
                    px[x, y] = (r, g, b, 0)
        return im

    temp24 = punch_black(temp24)
    hum24 = punch_black(hum24)
    # precip already has alpha; keep as-is

    temp24.save(out / "temperature.png")
    hum24.save(out / "humidity.png")
    precip24.save(out / "precip.png")

    to_lvgl_c(temp24, "fan_holo_icon_temp", out / "fan_holo_icon_temp.c", out / "fan_holo_icon_temp.h")
    to_lvgl_c(hum24, "fan_holo_icon_humidity", out / "fan_holo_icon_humidity.c", out / "fan_holo_icon_humidity.h")
    to_lvgl_c(precip24, "fan_holo_icon_precip", out / "fan_holo_icon_precip.c", out / "fan_holo_icon_precip.h")
    print("done")


if __name__ == "__main__":
    main()
