#!/usr/bin/env python3
"""Check SDK contents and Docker argument handling without building dependencies."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


BUILDER = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("package_mesa", BUILDER / "package-mesa.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class DockerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="melee sdk test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.mesa = self.root / "Mesa build with spaces"
        for relative in (package.STUBS, *package.REQUIRED_ARCHIVES):
            path = self.mesa / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"!<arch>\n" if path.suffix == ".a" else b"/* ABI bridge */\n")

    def test_context_contains_only_link_inputs_and_verifiable_manifest(self):
        extra = self.mesa / "builddir-switch/src/util/libmesa_util.a"
        extra.parent.mkdir(parents=True)
        extra.write_bytes(b"!<arch>\n")
        (extra.parent / "libsanity_check_for_rust.a").write_bytes(b"excluded")
        (self.mesa / "private-notes.txt").write_text("not a build input")
        output = io.BytesIO()
        package.write_context(self.mesa, output, "sha256:toolchain")
        with tarfile.open(fileobj=io.BytesIO(output.getvalue())) as archive:
            manifest = json.load(archive.extractfile("mesa-nvk/manifest.json"))
            self.assertEqual(manifest["toolchain_image_id"], "sha256:toolchain")
            self.assertEqual(len(manifest["files"]), 4)
            expected = {"Dockerfile", "mesa-nvk/manifest.json", "mesa-nvk/SHA256SUMS"}
            for item in manifest["files"]:
                name = "mesa-nvk/" + item["path"]
                expected.add(name)
                data = archive.extractfile(name).read()
                self.assertEqual(data, (self.mesa / item["path"]).read_bytes())
                self.assertEqual(hashlib.sha256(data).hexdigest(), item["sha256"])
            self.assertEqual(set(archive.getnames()), expected)
        repeated = io.BytesIO()
        package.write_context(self.mesa, repeated, "sha256:toolchain")
        self.assertEqual(output.getvalue(), repeated.getvalue())

    def test_rejects_missing_and_invalid_archives_before_streaming(self):
        archive = self.mesa / package.REQUIRED_ARCHIVES[0]
        for data in (b"", b"not an archive"):
            archive.write_bytes(data)
            output = io.BytesIO()
            with self.assertRaisesRegex(ValueError, "Not an archive"):
                package.write_context(self.mesa, output, "test")
            self.assertEqual(output.getvalue(), b"")
        archive.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            package.collect_inputs(self.mesa)

    def test_thin_archive_members_keep_their_paths_and_bytes(self):
        archive = self.mesa / package.REQUIRED_ARCHIVES[0]
        archive.unlink()
        member = archive.parent / "objects" / "member.o"
        member.parent.mkdir()
        member.write_bytes(b"member object contents")
        subprocess.run(["ar", "crT", archive.name, "objects/member.o"], cwd=archive.parent, check=True)
        original_archive = archive.read_bytes()
        output = io.BytesIO()
        package.write_context(self.mesa, output, "test")
        with tarfile.open(fileobj=io.BytesIO(output.getvalue())) as context:
            for path in (archive, member):
                copied = context.extractfile("mesa-nvk/" + path.relative_to(self.mesa).as_posix()).read()
                self.assertEqual(copied, path.read_bytes())
        self.assertEqual(archive.read_bytes(), original_archive)
        member.unlink()
        with self.assertRaises((OSError, subprocess.CalledProcessError)):
            package.collect_inputs(self.mesa)

    def test_rejects_nonrelocatable_thin_members(self):
        archive = self.mesa / package.REQUIRED_ARCHIVES[0]
        member = self.root / "outside.o"
        member.write_bytes(b"outside the SDK")
        for name in (str(member), os.path.relpath(member, archive.parent)):
            archive.unlink()
            subprocess.run(["ar", "crT", archive.name, name], cwd=archive.parent, check=True)
            with self.assertRaisesRegex(ValueError, "absolute member|outside Mesa"):
                package.collect_inputs(self.mesa)

    def run_wrapper(self, *args, **overrides):
        mock = self.root / "docker"
        mock.write_text(
            '#!/usr/bin/env bash\n'
            'printf "%s\\0" "$@" >> "$DOCKER_LOG"\n'
            'printf "\\0" >> "$DOCKER_LOG"\n'
            'if [[ "$1 $2" == "image inspect" ]]; then echo sha256:test; fi\n'
        )
        mock.chmod(0o755)
        log = self.root / "docker-arguments"
        log.write_bytes(b"")
        env = {
            key: value for key, value in os.environ.items()
            if not key.startswith("MELEE_") and key != "MESA_NVK_ROOT"
        }
        env.update(PATH=str(self.root) + os.pathsep + env["PATH"], DOCKER_LOG=str(log))
        env.update(overrides)
        result = subprocess.run(
            ["bash", str(BUILDER / "docker.sh"), *args], env=env, capture_output=True, text=True
        )
        calls = [call.decode().split("\0") for call in log.read_bytes().split(b"\0\0") if call]
        return result, calls

    def test_external_mesa_mount_and_build_settings(self):
        result, calls = self.run_wrapper(
            "melee", "configure", MESA_NVK_ROOT=str(self.mesa),
            MELEE_DOCKER_IMAGE="existing-toolchain", MELEE_BUILD_JOBS="8", MELEE_SDL_VARIANT="legacy"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        run = calls[-1]
        self.assertIn(str(self.mesa) + ":/mesa-nvk:ro", run)
        self.assertIn("MELEE_BUILD_JOBS=8", run)
        self.assertIn("MELEE_SDL_VARIANT=legacy", run)
        self.assertIn("existing-toolchain:latest", run)
        self.assertEqual(run[-4:], ["required", "bash", "builder/build-melee.sh", "configure"])

    def test_sdk_build_uses_no_host_mesa_mount(self):
        result, calls = self.run_wrapper("melee", MELEE_DOCKER_IMAGE="melee-nx-sdk:stable")
        self.assertEqual(result.returncode, 0, result.stderr)
        run = calls[-1]
        self.assertEqual(run.count("-v"), 1)
        self.assertIn("melee-nx-sdk:stable", run)
        self.assertEqual(run[-1], "all")

    def test_graphics_does_not_require_mesa(self):
        result, calls = self.run_wrapper("graphics", "prepare")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[-1][-4:], ["optional", "bash", "builder/build-graphics.sh", "prepare"])

    def test_invalid_path_and_command_do_not_start_containers(self):
        result, calls = self.run_wrapper("melee", MESA_NVK_ROOT=str(self.root / "missing"))
        self.assertEqual(result.returncode, 66)
        self.assertEqual(calls, [])
        result, calls = self.run_wrapper("invalid")
        self.assertEqual(result.returncode, 64)
        self.assertEqual(calls, [])

    def test_help_does_not_contact_docker(self):
        result, calls = self.run_wrapper("--help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("sdk-image", result.stdout)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
