#!/usr/bin/env python3
"""Check builder/docker.sh argument handling with a stand-in docker, without building anything."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


BUILDER = Path(__file__).resolve().parent


class DockerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="melee docker test ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

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
        env = {key: value for key, value in os.environ.items() if not key.startswith("MELEE_")}
        env.update(PATH=str(self.root) + os.pathsep + env["PATH"], DOCKER_LOG=str(log))
        env.update(overrides)
        result = subprocess.run(
            ["bash", str(BUILDER / "docker.sh"), *args], env=env, capture_output=True, text=True
        )
        calls = [call.decode().split("\0") for call in log.read_bytes().split(b"\0\0") if call]
        return result, calls

    def test_build_settings_reach_the_container(self):
        result, calls = self.run_wrapper(
            "melee", "configure",
            MELEE_DOCKER_IMAGE="existing-toolchain", MELEE_BUILD_JOBS="8", MELEE_SDL_VARIANT="legacy"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        run = calls[-1]
        self.assertEqual(run.count("-v"), 1)
        self.assertIn("MELEE_BUILD_JOBS=8", run)
        self.assertIn("MELEE_SDL_VARIANT=legacy", run)
        self.assertIn("existing-toolchain:latest", run)
        self.assertEqual(run[-3:], ["bash", "builder/build-melee.sh", "configure"])

    def test_graphics_stage(self):
        result, calls = self.run_wrapper("graphics", "prepare")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[-1][-3:], ["bash", "builder/build-graphics.sh", "prepare"])

    def test_invalid_command_does_not_start_containers(self):
        result, calls = self.run_wrapper("sdk-image")
        self.assertEqual(result.returncode, 64)
        self.assertEqual(calls, [])

    def test_help_does_not_contact_docker(self):
        result, calls = self.run_wrapper("help")
        self.assertEqual(result.returncode, 0)
        self.assertIn("graphics", result.stdout)
        self.assertNotIn("sdk-image", result.stdout)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
