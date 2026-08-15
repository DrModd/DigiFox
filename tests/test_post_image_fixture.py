#!/usr/bin/env python3
"""Behavioral tests for the Luckfox post-image release gate."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "ext_tree/board/luckfox/scripts/post-image.sh"
ENV_SOURCE = ROOT / "ext_tree/board/luckfox/config/uboot-env.txt"


class PostImageFixtureTests(unittest.TestCase):
    def run_hook(
        self, binaries: Path, host_dir: Path | None = None
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        if host_dir is not None:
            environment["HOST_DIR"] = str(host_dir)
        return subprocess.run(
            ["sh", str(SCRIPT), str(binaries)],
            text=True,
            capture_output=True,
            env=environment,
            check=False,
        )

    def test_renames_required_release_artifacts(self):
        with tempfile.TemporaryDirectory() as tempdir:
            binaries = Path(tempdir)
            (binaries / "rootfs.ubi").write_bytes(b"rootfs")
            (binaries / "uboot-env.bin").write_bytes(b"env")
            (binaries / "temporary.dtb").write_bytes(b"dtb")
            (binaries / "rootfs.ubifs").write_bytes(b"ubifs")

            result = self.run_hook(binaries)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((binaries / "rootfs.img").read_bytes(), b"rootfs")
            self.assertEqual((binaries / "env.img").read_bytes(), b"env")
            self.assertFalse((binaries / "rootfs.ubi").exists())
            self.assertFalse((binaries / "uboot-env.bin").exists())
            self.assertFalse((binaries / "temporary.dtb").exists())
            self.assertFalse((binaries / "rootfs.ubifs").exists())

            (binaries / "rootfs.ubi").write_bytes(b"rootfs")
            (binaries / "uboot-env.bin").write_bytes(b"env")
            second_result = self.run_hook(binaries)
            self.assertEqual(second_result.returncode, 0, second_result.stderr)
            self.assertEqual((binaries / "rootfs.img").read_bytes(), b"rootfs")
            self.assertEqual((binaries / "env.img").read_bytes(), b"env")
            self.assertFalse((binaries / "rootfs.ubi").exists())
            self.assertFalse((binaries / "uboot-env.bin").exists())

    def test_fails_without_rootfs_release_input(self):
        with tempfile.TemporaryDirectory() as tempdir:
            binaries = Path(tempdir)
            (binaries / "uboot-env.bin").write_bytes(b"env")

            result = self.run_hook(binaries)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("rootfs.ubi", result.stderr)
            self.assertTrue((binaries / "uboot-env.bin").exists())
            self.assertFalse((binaries / "rootfs.img").exists())

    def test_regenerates_missing_uboot_environment_from_external_source(self):
        self.assertTrue(ENV_SOURCE.is_file(), ENV_SOURCE)
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            binaries = root / "images"
            host_dir = root / "host"
            mkenvimage = host_dir / "bin/mkenvimage"
            binaries.mkdir()
            mkenvimage.parent.mkdir(parents=True)
            mkenvimage.write_text(
                "#!/bin/sh\n"
                "set -eu\n"
                "output=\n"
                "while [ \"$#\" -gt 0 ]; do\n"
                "    case \"$1\" in\n"
                "        -o) output=$2; shift 2 ;;\n"
                "        *) shift ;;\n"
                "    esac\n"
                "done\n"
                "[ -n \"$output\" ]\n"
                "cat > \"$output\"\n"
            )
            mkenvimage.chmod(0o755)
            (binaries / "rootfs.ubi").write_bytes(b"rootfs")

            result = self.run_hook(binaries, host_dir)

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((binaries / "env.img").read_bytes(), ENV_SOURCE.read_bytes())
            self.assertFalse((binaries / "rootfs.ubi").exists())
            self.assertFalse((binaries / "uboot-env.bin").exists())


if __name__ == "__main__":
    unittest.main()
