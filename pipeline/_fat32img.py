"""Write a sparse FAT32 disk image Windows will mount. No admin required."""
from __future__ import annotations

import ctypes
import os
from ctypes import wintypes
from pathlib import Path

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
GENERIC_WRITE = 0x40000000
GENERIC_READ = 0x80000000
OPEN_ALWAYS = 4
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
FSCTL_SET_SPARSE = 0x000900C4
INVALID_HANDLE = ctypes.c_void_p(-1).value


def _create_sparse_file(path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = kernel32.CreateFileW(
        str(path), GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_ALWAYS, 0, None
    )
    if handle == INVALID_HANDLE:
        raise OSError(ctypes.get_last_error(), "CreateFileW failed")
    try:
        returned = wintypes.DWORD(0)
        if not kernel32.DeviceIoControl(
            handle, FSCTL_SET_SPARSE, None, 0, None, 0, ctypes.byref(returned), None
        ):
            raise OSError(ctypes.get_last_error(), "FSCTL_SET_SPARSE failed")
        high = wintypes.DWORD((size >> 32) & 0xFFFFFFFF)
        kernel32.SetFilePointer(handle, size & 0xFFFFFFFF, ctypes.byref(high), 0)
        if not kernel32.SetEndOfFile(handle):
            raise OSError(ctypes.get_last_error(), "SetEndOfFile failed")
    finally:
        kernel32.CloseHandle(handle)


def _fat32_layout(size: int) -> tuple[int, int, int, int, int]:
    bytes_per_sec = 512
    reserved = 32
    total_sec = size // bytes_per_sec
    for sec_per_clus in (128, 64, 32, 16, 8, 4, 2, 1):
        fat_secs = 1
        for _ in range(16):
            data_sec = total_sec - reserved - 2 * fat_secs
            if data_sec <= 0:
                break
            clusters = data_sec // sec_per_clus
            need = ((clusters + 2) * 4 + bytes_per_sec - 1) // bytes_per_sec
            if need <= fat_secs:
                if 65525 <= clusters < 0x0FFFFFF5:
                    return bytes_per_sec, sec_per_clus, reserved, fat_secs, total_sec
                break
            fat_secs = need
    raise RuntimeError(f"could not converge FAT32 layout for size={size}")


def write_fat32_image(path: Path, size: int, label: str = "DLSSRAM") -> None:
    if size < 64 * 1024 * 1024:
        raise ValueError("FAT32 image must be at least 64 MB")
    bps, spc, reserved, fat_secs, total_sec = _fat32_layout(size)
    _create_sparse_file(path, size)

    label_bytes = f"{label:<11.11}".encode("ascii", "replace")
    boot = bytearray(512)
    boot[0:3] = b"\xEB\x58\x90"
    boot[3:11] = b"MSDOS5.0"
    boot[11:13] = bps.to_bytes(2, "little")
    boot[13] = spc
    boot[14:16] = reserved.to_bytes(2, "little")
    boot[16] = 2
    boot[21] = 0xF8
    boot[24:26] = (63).to_bytes(2, "little")
    boot[26:28] = (255).to_bytes(2, "little")
    boot[32:36] = total_sec.to_bytes(4, "little")
    boot[36:40] = fat_secs.to_bytes(4, "little")
    boot[44:48] = (2).to_bytes(4, "little")  # root cluster
    boot[48:50] = (1).to_bytes(2, "little")  # FSInfo
    boot[50:52] = (6).to_bytes(2, "little")  # backup boot
    boot[64] = 0x80
    boot[66] = 0x29
    boot[67:71] = b"\x12\x34\x56\x78"
    boot[71:82] = label_bytes
    boot[82:90] = b"FAT32   "
    boot[510:512] = b"\x55\xAA"

    fsinfo = bytearray(512)
    fsinfo[0:4] = b"\x52\x52\x61\x41"
    fsinfo[484:488] = b"\x72\x72\x41\x61"
    fsinfo[488:492] = (0xFFFFFFFF).to_bytes(4, "little")
    fsinfo[492:496] = (0xFFFFFFFF).to_bytes(4, "little")
    fsinfo[510:512] = b"\x55\xAA"

    clusters = (total_sec - reserved - 2 * fat_secs) // spc
    fat = bytearray(fat_secs * bps)
    fat[0:4] = b"\xF8\xFF\xFF\x0F"
    fat[4:8] = b"\xFF\xFF\xFF\x0F"
    fat[8:12] = b"\xFF\xFF\xFF\x0F"  # root cluster EOC
    del clusters  # layout already committed into fat_secs

    with path.open("r+b") as fh:
        fh.write(boot)
        fh.write(fsinfo)
        fh.seek(6 * bps)
        fh.write(boot)
        fh.seek(reserved * bps)
        fh.write(fat)
        fh.write(fat)
        os.fsync(fh.fileno())
