"""ImDisk RAM-disk helper for pipeline-v3.

ImDisk is the recommended backend, not a hard compile-time dependency.
If it is missing, fail with a clear install message — never pretend the
disk was created.

NTFS via `imdisk -p` needs an elevated token (`format.com`). No fallback
filesystem: if the format fails, report the permission problem clearly.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import shutil
import string
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

IMDISK_MISSING_MESSAGE = "请安装 ImDisk Toolkit"

# 16 GB, as specified for Stage 2 create(). Stage 3 must attach an already-mounted
# volume (default label DLSSRAM) and must not call create()/unmount().
DEFAULT_SIZE = "16G"
VOLUME_LABEL = "DLSSRAM"
PREFERRED_LETTERS = "RSTUVWXYZPQ"
DEFAULT_DISCOVER_LETTER = "R"

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


@dataclass
class RamDisk:
    letter: str
    size: str
    root: Path
    filesystem: str
    image_path: Path | None = None
    created: bool = True
    total_bytes: int = 0
    free_bytes: int = 0


class ImDiskError(RuntimeError):
    def __init__(self, message: str, *, missing: bool = False) -> None:
        super().__init__(message)
        self.missing = missing


def emit(event: str, **fields: object) -> None:
    payload = {"event": event, **fields}
    sys.stderr.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stderr.flush()


def find_imdisk() -> Path | None:
    which = shutil.which("imdisk")
    if which:
        return Path(which)
    for candidate in (
        Path(r"C:\Windows\System32\imdisk.exe"),
        Path(r"C:\Windows\SysWOW64\imdisk.exe"),
        Path(r"C:\Program Files\ImDisk\imdisk.exe"),
        Path(r"C:\Program Files (x86)\ImDisk\imdisk.exe"),
        Path(r"C:\Program Files\ImDisk Toolkit\imdisk.exe"),
        Path(r"C:\Program Files (x86)\ImDisk Toolkit\imdisk.exe"),
    ):
        if candidate.is_file():
            return candidate
    return None


def require_imdisk() -> Path:
    path = find_imdisk()
    if path is None:
        raise ImDiskError(IMDISK_MISSING_MESSAGE, missing=True)
    return path


def parse_size(size: str) -> int:
    text = size.strip().upper()
    multipliers = {"B": 1, "K": 1024, "M": 1024**2, "G": 1024**3, "T": 1024**4}
    if text[-1] in multipliers:
        return int(float(text[:-1]) * multipliers[text[-1]])
    return int(text)


def logical_letters() -> set[str]:
    mask = kernel32.GetLogicalDrives()
    return {chr(ord("A") + i) for i in range(26) if mask & (1 << i)}


def volume_filesystem(letter: str) -> str | None:
    fsname = ctypes.create_unicode_buffer(32)
    ok = kernel32.GetVolumeInformationW(
        f"{letter}:\\", None, 0, None, None, None, fsname, 32
    )
    if not ok:
        return None
    return fsname.value or ""


def volume_space(letter: str) -> tuple[int, int]:
    """Return (total_bytes, free_bytes) for a mounted volume. Never creates."""
    letter = letter.strip().rstrip(":").upper()
    free_avail = ctypes.c_ulonglong(0)
    total = ctypes.c_ulonglong(0)
    total_free = ctypes.c_ulonglong(0)
    ok = kernel32.GetDiskFreeSpaceExW(
        f"{letter}:\\",
        ctypes.byref(free_avail),
        ctypes.byref(total),
        ctypes.byref(total_free),
    )
    if not ok:
        raise ImDiskError(f"cannot query free space on {letter}:")
    return int(total.value), int(free_avail.value)


def volume_label(letter: str) -> str | None:
    letter = letter.strip().rstrip(":").upper()
    volname = ctypes.create_unicode_buffer(32)
    fsname = ctypes.create_unicode_buffer(32)
    ok = kernel32.GetVolumeInformationW(
        f"{letter}:\\", volname, 32, None, None, None, fsname, 32
    )
    if not ok:
        return None
    return volname.value or ""


def discover(
    *,
    letter: str | None = None,
    label: str = VOLUME_LABEL,
) -> RamDisk:
    """Find an already-mounted RAM disk. Never create, never unmount.

    Stage 3 hard constraint: the member-mounted ImDisk (label DLSSRAM,
    default R:) stays up for the whole session. Release GUI/CLI will need
    an elevated token for `imdisk -p` NTFS format; this helper does not
    request UAC and does not format.
    """
    used = logical_letters()
    wanted = (letter or "").strip().rstrip(":").upper() or None
    matches: list[str] = []
    for ch in sorted(used):
        vol = volume_label(ch)
        if vol is not None and vol.upper() == label.upper():
            matches.append(ch)

    chosen: str | None = None
    if wanted:
        if wanted not in used or volume_filesystem(wanted) is None:
            raise ImDiskError(f"{wanted}: 未挂载")
        vol = volume_label(wanted)
        if vol is not None and vol.upper() == label.upper():
            chosen = wanted
        elif not matches:
            # Issue: 卷标 DLSSRAM 或当前挂着的那块
            chosen = wanted
            emit(
                "ramdisk_label_mismatch",
                letter=wanted,
                label=vol or "",
                expected=label,
            )
        elif wanted in matches:
            chosen = wanted
        else:
            raise ImDiskError(f"{wanted}: 卷标不是 {label}")
    elif matches:
        preferred = DEFAULT_DISCOVER_LETTER
        chosen = preferred if preferred in matches else matches[0]
    else:
        raise ImDiskError(f"未找到 {label}")

    fs = volume_filesystem(chosen) or ""
    root = Path(f"{chosen}:\\")
    try:
        total_bytes, free_bytes = volume_space(chosen)
    except ImDiskError:
        total_bytes, free_bytes = 0, 0
    emit(
        "ramdisk_discovered",
        letter=chosen,
        root=str(root),
        filesystem=fs,
        label=volume_label(chosen) or "",
        created=False,
        total_bytes=total_bytes,
        free_bytes=free_bytes,
        message=f"已挂载内存盘 {chosen}:，共 {total_bytes} 字节，剩余 {free_bytes} 字节",
    )
    return RamDisk(
        letter=chosen,
        size="existing",
        root=root,
        filesystem=fs,
        created=False,
        total_bytes=total_bytes,
        free_bytes=free_bytes,
    )


def pick_letter(requested: str | None = None) -> str:
    used = logical_letters()
    if requested:
        letter = requested.strip().rstrip(":").upper()
        if len(letter) != 1 or letter not in string.ascii_uppercase:
            raise ImDiskError(f"invalid drive letter: {requested}")
        if letter in used and volume_filesystem(letter) is not None:
            raise ImDiskError(f"drive letter {letter}: is already in use")
        return letter
    for ch in PREFERRED_LETTERS:
        if ch not in used:
            return ch
    raise ImDiskError("no free drive letter for ImDisk RAM disk")


def _run_imdisk(imdisk: Path, args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(imdisk), *args],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def _imdisk_units(imdisk: Path) -> list[int]:
    result = _run_imdisk(imdisk, ["-l", "-n"])
    units: list[int] = []
    for line in (result.stdout or "").splitlines():
        text = line.strip()
        if text.isdigit():
            units.append(int(text))
    if not units:
        for line in (result.stdout or result.stderr or "").splitlines():
            if "ImDisk" in line:
                tail = line.rsplit("ImDisk", 1)[-1].strip()
                if tail.isdigit():
                    units.append(int(tail))
    return units


def _force_remove_units(imdisk: Path) -> None:
    for unit in _imdisk_units(imdisk):
        _run_imdisk(imdisk, ["-D", "-u", str(unit)])
        _run_imdisk(imdisk, ["-R", "-u", str(unit)])


def unmount(letter: str, *, force: bool = False, missing_ok: bool = False) -> None:
    """Unmount an ImDisk volume. Tries graceful, then forced, even after errors."""
    imdisk = find_imdisk()
    if imdisk is None:
        if missing_ok:
            emit("ramdisk_unmount_skipped", reason=IMDISK_MISSING_MESSAGE)
            return
        raise ImDiskError(IMDISK_MISSING_MESSAGE, missing=True)
    letter = letter.strip().rstrip(":").upper()
    emit("ramdisk_unmount", letter=letter, force=force)
    flags = ["-D", "-m", f"{letter}:"] if force else ["-d", "-m", f"{letter}:"]
    result = _run_imdisk(imdisk, flags)
    if letter in logical_letters():
        result = _run_imdisk(imdisk, ["-D", "-m", f"{letter}:"])
    _force_remove_units(imdisk)
    still_there = letter in logical_letters() and volume_filesystem(letter) is not None
    if still_there and not missing_ok:
        stderr = (result.stderr or result.stdout or "").strip()
        raise ImDiskError(
            f"ImDisk failed to unmount {letter}: (exit {result.returncode}) {stderr}"
        )
    emit("ramdisk_unmounted", letter=letter)


def _try_ntfs(imdisk: Path, letter: str, size: str) -> bool:
    emit("ramdisk_ntfs_probe", letter=letter)
    probe = _run_imdisk(
        imdisk,
        [
            "-a", "-t", "vm", "-s", "64M", "-m", f"{letter}:",
            "-p", "/fs:ntfs /q /y /v:DLSSRAM",
        ],
    )
    probe_fs = volume_filesystem(letter)
    probe_log = (probe.stderr or probe.stdout or "").strip()
    emit(
        "ramdisk_ntfs_probe_result",
        exit=probe.returncode,
        filesystem=probe_fs,
        log=probe_log[-400:] if probe_log else "",
    )
    unmount(letter, force=True, missing_ok=True)
    if probe_fs is None or probe_fs.upper() != "NTFS":
        return False

    emit("ramdisk_ntfs_format", letter=letter, size=size)
    result = _run_imdisk(
        imdisk,
        [
            "-a", "-t", "vm", "-s", size, "-m", f"{letter}:",
            "-p", "/fs:ntfs /q /y /v:DLSSRAM",
        ],
    )
    fs = volume_filesystem(letter)
    log = (result.stderr or result.stdout or "").strip()
    emit(
        "ramdisk_ntfs_format_result",
        exit=result.returncode,
        filesystem=fs,
        log=log[-400:] if log else "",
    )
    return fs is not None and fs.upper() == "NTFS"


def _try_image(imdisk: Path, letter: str, size: str) -> Path:
    nbytes = parse_size(size)
    image = Path(tempfile.gettempdir()) / f"dlss-ramdisk-{letter}.img"
    emit("ramdisk_image_write", path=str(image), bytes=nbytes)
    write_fat32_image(image, nbytes, "DLSSRAM")
    result = _run_imdisk(
        imdisk,
        ["-a", "-t", "vm", "-f", str(image), "-m", f"{letter}:"],
    )
    fs = volume_filesystem(letter)
    if result.returncode != 0 or fs is None:
        log = (result.stderr or result.stdout or "").strip()
        unmount(letter, force=True, missing_ok=True)
        image.unlink(missing_ok=True)
        raise ImDiskError(
            f"ImDisk failed to attach {size} image on {letter}: "
            f"(exit {result.returncode}) {log}"
        )
    try:
        image.unlink()
        image = Path()  # gone; RAM copy remains
        emit("ramdisk_image_deleted_after_load", letter=letter)
        return Path()
    except OSError:
        emit("ramdisk_image_locked", path=str(image))
        return image


def create(size: str = DEFAULT_SIZE, letter: str | None = None) -> RamDisk:
    """Create a RAM disk formatted NTFS. Needs an elevated token for format.com."""
    imdisk = require_imdisk()
    chosen = pick_letter(letter)
    emit("ramdisk_create", letter=chosen, size=size, imdisk=str(imdisk))
    try:
        if not _try_ntfs(imdisk, chosen, size):
            unmount(chosen, force=True, missing_ok=True)
            raise ImDiskError(
                "内存盘创建失败：NTFS 快速格式化需要管理员权限（format.com）。"
                "请以管理员身份运行，或改用缓存路径模式。"
            )
        fs = volume_filesystem(chosen)
        if fs is None:
            raise ImDiskError(f"ImDisk attached {chosen}: but no filesystem")
        root = Path(f"{chosen}:\\")
        emit("ramdisk_ready", letter=chosen, root=str(root), filesystem=fs)
        return RamDisk(letter=chosen, size=size, root=root, filesystem=fs)
    except Exception:
        unmount(chosen, force=True, missing_ok=True)
        raise


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="ImDisk RAM-disk helper (pipeline-v3)")
    parser.add_argument("action", choices=("status", "create", "unmount", "discover"))
    parser.add_argument("--letter", default=None)
    parser.add_argument("--size", default=DEFAULT_SIZE)
    parser.add_argument("--label", default=VOLUME_LABEL)
    args = parser.parse_args(argv)

    try:
        if args.action == "status":
            path = find_imdisk()
            if path is None:
                emit("imdisk_missing", message=IMDISK_MISSING_MESSAGE)
                print(IMDISK_MISSING_MESSAGE)
                return 2
            emit("imdisk_ok", path=str(path))
            mounted: list[dict[str, object]] = []
            for ch in sorted(logical_letters()):
                vol = volume_label(ch)
                fs = volume_filesystem(ch)
                if vol is None and fs is None:
                    continue
                mounted.append({"letter": ch, "label": vol or "", "filesystem": fs or ""})
            emit("ramdisk_volumes", volumes=mounted)
            print(path)
            return 0
        if args.action == "discover":
            disk = discover(letter=args.letter, label=args.label)
            print(disk.root)
            return 0
        if args.action == "create":
            disk = create(size=args.size, letter=args.letter)
            print(disk.root)
            return 0
        if args.letter is None:
            raise ImDiskError("--letter is required for unmount")
        unmount(args.letter)
        return 0
    except ImDiskError as exc:
        emit("error", message=str(exc), missing=exc.missing)
        return 2 if exc.missing else 1


if __name__ == "__main__":
    sys.exit(main())
