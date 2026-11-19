#!/usr/bin/env python3
"""Replaces Git LFS pointer files in a checkout with their content, without git-lfs.

Each file comes from GitHub's media endpoint at the pinned commit, and its SHA-256 must match the
oid in the pointer (the pointer is the checksum, so a pinned commit pins the content too).

    tools/lfs-fetch.py <checkout> <owner/repo> <commit> <path>...

A path may be a directory: every pointer file under it is fetched, except Blender and GIMP sources
(.blend, .xcf), which no build here needs. Files that are already fetched are skipped.

SPDX-FileCopyrightText: 2026 Mitul Shah
SPDX-License-Identifier: MIT
"""
import concurrent.futures
import hashlib
import pathlib
import sys
import urllib.parse
import urllib.request

SKIP = {".blend", ".xcf", ".mp4", ".webm"}
POINTER = b"version https://git-lfs.github.com/spec/v1"


def pointers(root, rel):
    p = root / rel
    files = [p] if p.is_file() else sorted(f for f in p.rglob("*") if f.is_file())
    for f in files:
        if f.suffix in SKIP or f.stat().st_size > 1024:
            continue
        if f.read_bytes().startswith(POINTER):
            yield f


def fetch(root, repo, commit, f):
    oid = next(l.split(b":", 1)[1].decode() for l in f.read_bytes().splitlines() if l.startswith(b"oid sha256:"))
    rel = f.relative_to(root).as_posix()
    url = f"https://media.githubusercontent.com/media/{repo}/{commit}/{urllib.parse.quote(rel)}"
    data = urllib.request.urlopen(url, timeout=120).read()
    if hashlib.sha256(data).hexdigest() != oid:
        raise RuntimeError(f"{rel}: sha256 mismatch")
    f.write_bytes(data)
    return len(data)


def main():
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    root, repo, commit = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    todo = [f for rel in sys.argv[4:] for f in pointers(root, rel)]
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        total = sum(ex.map(lambda f: fetch(root, repo, commit, f), todo))
    print(f"lfs-fetch: {len(todo)} files, {total / 1048576:.1f} MB, checksums verified")


if __name__ == "__main__":
    main()
