#!/usr/bin/env python3
"""
emotion_bin_packer.py

把一组 mjpeg 文件打包成 emotion_partition_storage 兼容的 .bin 镜像。

布局（与 main/utils/emotion_partition_storage.c 一致）：
  [Header 64B] @ offset 0
    magic       uint32 = 'APSV' (0x56535041, little-endian)
    version     uint8  = 1
    entry_count uint8  = 文件数
    reserved    uint8  = 0
    reserved    uint8  = 0
    deleted_count uint32 = 0
    reserved    [HEADER_SIZE - 12] = 0xff

  [Entry 64B] @ 64 + i*64,  i = 0..n-1
    name[32]    char[]
    offset      uint32  数据区偏移（相对分区）
    size        uint32  实际数据大小（不含 padding）
    crc32       uint32  esp_rom_crc32_le(data)
    flags       uint8   0 = valid, 1 = deleted
    reserved    [19]    0x00

  [Data ...]  按 4KB 对齐追加，每段填 0xff 至 4KB 边界

启动时 emotion_partition_storage_init 会：
  - 读 header@0 (sizeof(emotion_header_t))
  - 读 entry_count*64 @ 64
  - flags == 0 (EMOTION_FLAG_VALID) 的 entry 视为有效

注意：
- 分区大小一般 6MB+，因此 bin 可以仅包含 header + entries + data 的"最小可用镜像"。
- 烧录时 esptool write_flash 会把 bin 外的字节擦成 0xff，但 emotion_partition_storage
  只依赖 header/entries/data 本身，不依赖其他字节。
- data 区域 4KB 对齐，padded 至 4KB 边界（与 flash_write_padded 行为一致）。
"""

import os
import struct
import sys
import zlib


# 与 emotion_partition_storage.c 保持一致
HEADER_SIZE = 64
ENTRY_SIZE = 64
NAME_MAX = 32
DATA_RESERVE = 0x1000  # 4 KB - header + entries 共用一个 sector
DATA_ALIGN = 0x1000    # 4 KB
MAX_ENTRIES = 32       # 实际只用前 N 个

# magic 'APSV' = 0x56535041
EMOTION_STORAGE_HEADER_MAGIC = 0x56535041
EMOTION_FLAG_VALID = 0
EMOTION_FLAG_DELETED = 1


def _align_up(v: int, a: int) -> int:
    return (v + (a - 1)) & ~(a - 1)


def build_emotion_bin(files: list, output_path: str) -> None:
    """
    files: [(asset_name: str, mjpeg_path: str), ...]
        asset_name 必须 <= 31 字节（含 '\0'），例如 "default-idle-240x290.mjpeg"
    output_path: 输出 .bin 文件路径
    """
    if not files:
        sys.exit("[错误] build_emotion_bin: files 为空")
    if len(files) > MAX_ENTRIES:
        sys.exit(f"[错误] 文件数 {len(files)} 超过 MAX_ENTRIES={MAX_ENTRIES}")

    # 检查名称与文件
    entries = []
    for asset_name, mjpeg_path in files:
        if len(asset_name.encode("utf-8")) >= NAME_MAX:
            sys.exit(f"[错误] asset_name '{asset_name}' 超过 31 字节（含 \\0）")
        if not os.path.exists(mjpeg_path):
            sys.exit(f"[错误] 找不到 mjpeg 文件: {mjpeg_path}")
        with open(mjpeg_path, "rb") as f:
            data = f.read()
        if not data:
            sys.exit(f"[错误] mjpeg 文件为空: {mjpeg_path}")
        entries.append({
            "name": asset_name,
            "size": len(data),
            "crc32": zlib.crc32(data) & 0xFFFFFFFF,
            "data": data,
        })

    # 计算每个 entry 的 data 起始 offset（4KB 对齐）
    cur_off = DATA_RESERVE
    for e in entries:
        e["offset"] = cur_off
        cur_off += _align_up(e["size"], DATA_ALIGN)
    total_size = cur_off

    # 构造 buffer
    buf = bytearray(b"\xff" * total_size)

    # ---- Header (64B) ----
    struct.pack_into("<I", buf, 0, EMOTION_STORAGE_HEADER_MAGIC)
    buf[4] = 1                              # version
    buf[5] = len(entries) & 0xFF            # entry_count
    buf[6] = 0                              # reserved0
    buf[7] = 0                              # reserved1
    struct.pack_into("<I", buf, 8, 0)       # deleted_count = 0
    # 12..64 reserved (0xff)

    # ---- Entries ----
    for i, e in enumerate(entries):
        off = HEADER_SIZE + i * ENTRY_SIZE
        # name 必须以 '\0' 结尾，否则 C 端 strcmp 会读到结构体外（C 端按 name[32] 紧接
        # 的是 offset/size/crc32 字段，恰好不是 0 字节，会越界找 '\0'）。
        # 显式先填 0，再覆盖 name 字节。
        struct.pack_into("32s", buf, off, b"")
        name_bytes = e["name"].encode("utf-8")
        buf[off:off + len(name_bytes)] = name_bytes
        buf[off + len(name_bytes)] = 0  # name 终止符
        struct.pack_into("<I", buf, off + 32, e["offset"])
        struct.pack_into("<I", buf, off + 36, e["size"])
        struct.pack_into("<I", buf, off + 40, e["crc32"])
        buf[off + 44] = EMOTION_FLAG_VALID   # 关键：flags = 0 (valid)
        # 45..64 reserved (0x00)

    # ---- Data ----
    for e in entries:
        buf[e["offset"]:e["offset"] + e["size"]] = e["data"]

    # 写文件
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    with open(output_path, "wb") as f:
        f.write(buf)

    print(f"[OK] 已生成 emotion bin ({total_size} 字节): {output_path}")
    for e in entries:
        print(f"     - {e['name']}: off=0x{e['offset']:x} size={e['size']} crc32=0x{e['crc32']:08x}")


if __name__ == "__main__":
    # 简单 CLI 入口，方便单独测试
    import argparse
    ap = argparse.ArgumentParser(description="打包 mjpeg 列表到 emotion_partition_storage 兼容 .bin")
    ap.add_argument("--output", required=True, help="输出 .bin 路径")
    ap.add_argument("--file", action="append", required=True,
                    help="asset_name=mjpeg_path 形式，可多次传")
    args = ap.parse_args()
    files = []
    for spec in args.file:
        if "=" not in spec:
            sys.exit(f"[错误] --file 需要 name=path 格式: {spec}")
        name, path = spec.split("=", 1)
        files.append((name.strip(), path.strip()))
    build_emotion_bin(files, args.output)
