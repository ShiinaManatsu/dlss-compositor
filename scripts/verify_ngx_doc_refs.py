#!/usr/bin/env python3
"""
Verify that every NVSDK_NGX_* API call has a // DLSS § citation comment nearby.
Per v2 plan Must Have: every NGX call must have §X.X reference to prevent UE cargo-cult bugs.

Usage:
    python scripts/verify_ngx_doc_refs.py dlss-plugin/src/dlss/
    python scripts/verify_ngx_doc_refs.py dlss-plugin/src/dlss/ngx_wrapper.cpp
"""
import os
import re
import sys
from pathlib import Path


def verify_file(filepath: str) -> list[tuple[int, str]]:
    """Return list of (line_num, line_content) for NGX calls missing doc refs."""
    issues = []
    lines = Path(filepath).read_text(encoding="utf-8").splitlines()

    ngx_pattern = re.compile(r"\bNVSDK_NGX_\w+\s*[(\[]")
    ref_pattern = re.compile(r"//.*DLSS\s*[§S]")

    for i, line in enumerate(lines):
        if ngx_pattern.search(line):
            # Check this line and 5 lines before for a §-reference comment.
            start = max(0, i - 5)
            context = "\n".join(lines[start:i + 1])
            if not ref_pattern.search(context):
                issues.append((i + 1, line.strip()))

    return issues


def main():
    if len(sys.argv) < 2:
        print("Usage: verify_ngx_doc_refs.py <file_or_dir>")
        sys.exit(1)

    path = sys.argv[1]
    all_issues = []

    if os.path.isfile(path):
        files = [path]
    else:
        files = [str(p) for p in Path(path).rglob("*.cpp")]

    for f in files:
        issues = verify_file(f)
        for lineno, line in issues:
            print(f"{f}:{lineno}: missing DLSS § comment: {line[:80]}")
            all_issues.append((f, lineno, line))

    if all_issues:
        print(f"\n{len(all_issues)} NVSDK_NGX_* calls missing DLSS § citations!")
        sys.exit(1)

    print("All NVSDK_NGX_* calls have DLSS § citations ✓")
    sys.exit(0)


if __name__ == "__main__":
    main()
