#!/usr/bin/env python3
"""Stream the existing Switch Mesa link inputs as an SDK Docker build context."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tarfile


STUBS = Path("src/nouveau/vulkan/rust_switch_stubs.c")
REQUIRED_ARCHIVES = (
    Path("builddir-switch/src/nouveau/vulkan/libnvk.a"),
    Path("builddir-switch/src/nouveau/vulkan/libvulkan.a"),
)


def collect_inputs(root):
    root = root.resolve(strict=True)
    for path in (STUBS, *REQUIRED_ARCHIVES):
        if not (root / path).is_file():
            raise ValueError(f"Missing Mesa build input: {path}")

    # Match Graphics.cmake's archive selection and ordering.
    archives = sorted(
        path.relative_to(root)
        for path in (root / "builddir-switch").rglob("*.a")
        if path.name != "libsanity_check_for_rust.a"
    )
    paths = {STUBS, *archives}
    for relative in archives:
        path = root / relative
        with path.open("rb") as stream:
            header = stream.read(8)
        if header == b"!<thin>\n":
            members = subprocess.check_output(
                ["ar", "t", path.name], cwd=path.parent, text=True, stderr=subprocess.PIPE
            )
            for name in members.splitlines():
                member = Path(name)
                if member.is_absolute():
                    raise ValueError(f"Thin archive has an absolute member path: {relative}: {name}")
                resolved = (path.parent / member).resolve(strict=True)
                if not resolved.is_relative_to(root):
                    raise ValueError(f"Thin archive member is outside Mesa: {relative}: {name}")
                paths.add(resolved.relative_to(root))
        elif header != b"!<arch>\n":
            raise ValueError(f"Not an archive: {relative}")
    records = []
    for relative in sorted(paths):
        path = root / relative
        if not path.resolve(strict=True).is_relative_to(root):
            raise ValueError(f"Mesa input points outside its root: {relative}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        records.append({"path": relative.as_posix(), "sha256": digest, "bytes": path.stat().st_size})
    return root, records


def add_bytes(archive, name, content):
    entry = tarfile.TarInfo(name)
    entry.size = len(content)
    entry.mode = 0o644
    archive.addfile(entry, io.BytesIO(content))


def write_context(root, output, toolchain_id):
    root, records = collect_inputs(root)
    manifest = {"format": 1, "toolchain_image_id": toolchain_id, "files": records}
    checksums = "".join(f"{item['sha256']}  {item['path']}\n" for item in records)
    dockerfile = Path(__file__).resolve().parents[1] / "switch/docker/Dockerfile.sdk"
    with tarfile.open(fileobj=output, mode="w|") as archive:
        add_bytes(archive, "Dockerfile", dockerfile.read_bytes())
        add_bytes(archive, "mesa-nvk/manifest.json", (json.dumps(manifest, indent=2) + "\n").encode())
        add_bytes(archive, "mesa-nvk/SHA256SUMS", checksums.encode())
        for item in records:
            path = root / item["path"]
            entry = tarfile.TarInfo("mesa-nvk/" + item["path"])
            entry.size = item["bytes"]
            entry.mode = 0o644
            with path.open("rb") as stream:
                archive.addfile(entry, stream)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mesa_root", type=Path)
    parser.add_argument("--toolchain-id", required=True)
    args = parser.parse_args()
    try:
        write_context(args.mesa_root, sys.stdout.buffer, args.toolchain_id)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Cannot package Mesa: {error}\n")


if __name__ == "__main__":
    main()
