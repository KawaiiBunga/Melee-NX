#!/usr/bin/env python3
"""Reconstruct Melee/Aurora and SDL patch files from pinned Git objects.

Never modifies a reference tree or its index. Also compares the reconstruction
to the live sources and reverse-checks from the same directories as the builder.
Run: python builder/verify-patches.py
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / "switch/patches"


def git(tree, *args, check=True):
    result = subprocess.run(["git", "-C", str(tree), *args], capture_output=True)
    if check and result.returncode:
        raise RuntimeError(result.stderr.decode(errors="replace"))
    return result


def normalize(data):
    return data.replace(b"\r\n", b"\n")


def paths(patch):
    return re.findall(r"^\+\+\+ b/(\S+)", patch.read_text(encoding="utf-8"), re.M)


def verify(work):
    pc = ROOT / "ref/melee-pc"
    revision = git(pc, "rev-parse", "HEAD").stdout.decode().strip()
    if revision != "7c9a468f4f8206780c4cd762be1da7772daaeabf":
        raise RuntimeError(f"Unexpected melee-pc revision: {revision}")
    clean = work / "melee"
    clean.mkdir()
    git(clean, "init", "-q")
    owned = set()
    operations = []
    for script in ["build-graphics.sh", "build-melee.sh"]:
        text = (ROOT / "builder" / script).read_text(encoding="utf-8")
        names = re.findall(r"switch/patches/((?:aurora|melee)-[\w-]+\.patch)", text)
        for name in dict.fromkeys(names):
            patch = PATCHES / name
            prefix = "extern/aurora/" if name.startswith("aurora-") else ""
            for path in paths(patch):
                full = prefix + path
                if full in owned:
                    raise RuntimeError(f"Overlapping patch ownership: {full}")
                owned.add(full)
                data = git(pc, "show", "HEAD:" + full).stdout
                target = clean / full
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(normalize(data))
            operations.append((name, prefix))

    for name, prefix in operations:
        patch = str(PATCHES / name)
        directory = ["--directory=" + prefix.rstrip("/")] if prefix else []
        git(clean, "apply", "--check", *directory, patch)
        git(clean, "apply", *directory, patch)
        git(clean, "apply", "--reverse", "--check", *directory, patch)
        # Real build working directory: prevents passing a check that skips all hunks.
        result = git(pc, "apply", "-p1", "--verbose", "--ignore-space-change",
                     "--reverse", "--check", *directory, patch)
        if b"Skipped patch" in result.stderr:
            raise RuntimeError(f"Builder silently skips hunks in {name}")
    for path in owned:
        if normalize((clean / path).read_bytes()) != normalize((pc / path).read_bytes()):
            raise RuntimeError(f"Source drift outside patches: {path}")
    stats = git(pc, "diff", "--ignore-space-at-eol", "--numstat").stdout.decode().splitlines()
    for line in stats:
        added, removed, path = line.split("\t")
        if (added != "0" or removed != "0") and path not in owned:
            raise RuntimeError(f"Uncaptured tracked source change: {path}")
    print(f"Melee/Aurora: {len(operations)} patches, {len(owned)} files; clean apply, live reverse and content match")

    sdl = ROOT / "ref/SDL-dusklight"
    if git(sdl, "rev-parse", "HEAD").stdout != git(sdl, "rev-parse", "release-3.4.10").stdout:
        raise RuntimeError("SDL is not at release-3.4.10")
    clean = work / "sdl"
    clean.mkdir()
    git(clean, "init", "-q")
    stack = [PATCHES / "sdl-dusklight-switch.patch", PATCHES / "sdl-dusklight-melee-nx.patch"]
    owned = set(path for patch in stack for path in paths(patch))
    for path in owned:
        data = git(sdl, "show", "HEAD:" + path, check=False)
        if data.returncode == 0:
            target = clean / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(normalize(data.stdout))
    for patch in stack:
        git(clean, "apply", "--recount", "--check", str(patch))
        git(clean, "apply", "--recount", str(patch))
    git(clean, "apply", "--recount", "--reverse", "--check", str(stack[-1]))
    git(sdl, "apply", "--recount", "--ignore-space-change", "--reverse", "--check", str(stack[-1]))
    for path in owned:
        if normalize((clean / path).read_bytes()) != normalize((sdl / path).read_bytes()):
            raise RuntimeError(f"SDL source drift: {path}")
    print(f"SDL 3.4.10: 2 patches, {len(owned)} files; clean apply, live reverse and content match")


if __name__ == "__main__":
    scratch = (ROOT / "scratch").resolve()
    scratch.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="verify-patches-", dir=scratch)).resolve()
    try:
        verify(work)
    finally:
        # Only delete this invocation's verified, isolated workspace directory.
        if work.parent != scratch or not work.name.startswith("verify-patches-"):
            raise RuntimeError(f"Unsafe temporary directory: {work}")
        shutil.rmtree(work)
