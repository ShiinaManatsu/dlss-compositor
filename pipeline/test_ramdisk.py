"""Unit checks for Stage 3 RAM-disk discover. Does not create or unmount."""
from __future__ import annotations

from pathlib import Path

from ramdisk import VOLUME_LABEL, discover, volume_label, volume_space
from rpc_client import encode_frame


def test_discover_missing_letter_does_not_create() -> None:
    from session_config import user_error_message

    try:
        discover(letter="?")
        raise AssertionError("invalid letter should fail")
    except Exception as exc:
        short = user_error_message(str(exc))
        assert "未挂载" in short
        assert "{" not in short
        assert "Stage 3" not in str(exc)


def test_encode_rpc_frame_content_length() -> None:
    payload = {"jsonrpc": "2.0", "id": 1, "method": "progress", "params": {}}
    frame = encode_frame(payload)
    assert frame.startswith(b"Content-Length: ")
    header, body = frame.split(b"\r\n\r\n", 1)
    length = int(header.split(b":", 1)[1].strip())
    assert length == len(body)


def test_discover_existing_dlssram_if_mounted() -> None:
    disk = None
    try:
        disk = discover(letter="R", label=VOLUME_LABEL)
    except Exception as exc:
        print(f"discover skipped: {exc}")
        return
    assert disk.created is False
    assert disk.root == Path("R:\\")
    label = volume_label("R")
    total, free = volume_space("R")
    assert total > 0
    assert free >= 0
    print(f"discovered {disk.letter}: label={label} fs={disk.filesystem} total={total} free={free}")


if __name__ == "__main__":
    test_encode_rpc_frame_content_length()
    test_discover_missing_letter_does_not_create()
    test_discover_existing_dlssram_if_mounted()
    print("test_ramdisk ok")
