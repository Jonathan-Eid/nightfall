#!/usr/bin/env python3
"""Exercise native_xr_cache.sh using temporary, synthetic toolchain fixtures."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
REQUIRED_FILES = (
    "templates/4.7.stable/android_release_arm64.so",
    "templates/4.7.stable/android_release_arm64.so.build-stamp",
    "godot-cpp/bin/libgodot-cpp.android.template_release.arm64.a",
    "godot-cpp/extension_api.json",
    "godot-cpp/gdextension/gdextension_interface.json",
    "godot-cpp/gen/include/godot_cpp/classes/open_xr_extension_wrapper_extension.hpp",
    "godot/thirdparty/openxr/include/openxr/openxr.h",
    "godot-cpp-api.version",
    "godot-patches.sha256",
)
PATCHES = (
    "godot-4.7-ahb.patch",
    "godot-4.7-projectionless.patch",
    "godot-4.7-projectionless-lifecycle.patch",
    "godot-4.7-compositor-filter.patch",
    "godot-4.7-gles3-render-target-override.patch",
)


def copy_lf(source: Path, destination: Path) -> None:
    """Copy text while removing Windows CRLF, for a Bash fixture on Linux."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read_bytes().replace(b"\r\n", b"\n"))


class NativeXRCacheTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="native-xr-cache-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "project"
        self.cache = Path(self.temporary.name) / "ci-cache"
        self.native = Path(self.temporary.name) / "native-xr"

        copy_lf(
            REPO_ROOT / "tools/build_support/native_xr_versions.sh",
            self.root / "tools/build_support/native_xr_versions.sh",
        )
        copy_lf(
            REPO_ROOT / "tools/ci/native_xr_cache.sh",
            self.root / "tools/ci/native_xr_cache.sh",
        )
        self.bootstrap = self.root / "tools/build_support/bootstrap_native_xr.sh"
        self.bootstrap.parent.mkdir(parents=True, exist_ok=True)
        self.bootstrap.write_text(
            "#!/usr/bin/env bash\n"
            "set -euo pipefail\n"
            ": \"${NIGHTFALL_BOOTSTRAP_ARGS:?}\"\n"
            "printf '%s\\n' \"$@\" > \"$NIGHTFALL_BOOTSTRAP_ARGS\"\n"
            "exit 77\n",
            encoding="utf-8",
        )
        self.bootstrap.chmod(0o755)

        patches_dir = self.root / "patches"
        patches_dir.mkdir(parents=True)
        for patch in PATCHES:
            (patches_dir / patch).write_text(f"synthetic fixture: {patch}\n", encoding="utf-8")

        self.environment = os.environ.copy()
        self.environment.update(
            {
                "NIGHTFALL_CI_PROJECT_ROOT": str(self.root),
                "NIGHTFALL_NATIVE_XR_CACHE": str(self.native),
                "NIGHTFALL_CI_CACHE": str(self.cache),
            }
        )
        self.helper = self.root / "tools/ci/native_xr_cache.sh"

    def run_helper(self, action: str, *, expected: int | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(
            ["bash", str(self.helper), action],
            env=self.environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if expected is not None:
            self.assertEqual(result.returncode, expected, result.stdout)
        return result

    def create_valid_fixture(self) -> None:
        for relative in REQUIRED_FILES:
            path = self.native / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(f"synthetic fixture: {relative}\n".encode())

        runtime = self.native / REQUIRED_FILES[0]
        stamp = subprocess.run(
            [
                "bash",
                "-c",
                'source "$1"; nightfall_runtime_build_stamp "$2" "$3" > "$3.build-stamp"',
                "native-xr-test",
                str(self.root / "tools/build_support/native_xr_versions.sh"),
                str(self.root),
                str(runtime),
            ],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.assertEqual(stamp.returncode, 0, stamp.stdout)

        versions = (self.root / "tools/build_support/native_xr_versions.sh").read_text(encoding="utf-8")
        godot_commit = self._assignment(versions, "NIGHTFALL_PINNED_GODOT_COMMIT")
        cpp_commit = self._assignment(versions, "NIGHTFALL_PINNED_GODOT_CPP_COMMIT")
        (self.native / "godot-cpp-api.version").write_text(
            f"Godot {godot_commit} / godot-cpp {cpp_commit}", encoding="utf-8"
        )
        self.write_manifest()

    @staticmethod
    def _assignment(source: str, name: str) -> str:
        prefix = f'{name}="'
        line = next(line for line in source.splitlines() if line.startswith(prefix))
        return line[len(prefix) : -1]

    def write_manifest(self) -> None:
        fingerprint = self.run_helper("fingerprint", expected=0).stdout.strip()
        lines = [f"inputs {fingerprint}"]
        for relative in REQUIRED_FILES:
            digest = hashlib.sha256((self.native / relative).read_bytes()).hexdigest()
            lines.append(f"{digest}  {relative}")
        manifest = self.native / "ci-release.sha256"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def test_valid_cache_hit_skips_bootstrap(self) -> None:
        self.create_valid_fixture()
        self.run_helper("ensure", expected=0)
        self.assertFalse((self.cache / "bootstrap-args.txt").exists())

    def test_missing_runtime_or_binding_library_is_rejected(self) -> None:
        self.create_valid_fixture()
        runtime = self.native / REQUIRED_FILES[0]
        runtime.unlink()
        self.assertEqual(self.run_helper("check").returncode, 1)

        self.create_valid_fixture()
        library = self.native / REQUIRED_FILES[2]
        library.unlink()
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_tampered_binding_library_is_rejected(self) -> None:
        self.create_valid_fixture()
        (self.native / REQUIRED_FILES[2]).write_bytes(b"tampered static binding library")
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_tampered_runtime_is_rejected(self) -> None:
        self.create_valid_fixture()
        (self.native / REQUIRED_FILES[0]).write_bytes(b"tampered runtime")
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_changed_engine_patch_invalidates_manifest(self) -> None:
        self.create_valid_fixture()
        (self.root / "patches" / PATCHES[0]).write_text("changed patch\n", encoding="utf-8")
        self.assertEqual(self.run_helper("check").returncode, 1)

    def test_archive_round_trip_preserves_inputs_and_excludes_objects(self) -> None:
        self.create_valid_fixture()
        (self.native / "godot-cpp/build/generated.o").parent.mkdir(parents=True, exist_ok=True)
        (self.native / "godot-cpp/build/generated.o").write_bytes(b"object")
        (self.native / "godot-cpp/build/generated.os").write_bytes(b"object")
        self.run_helper("pack", expected=0)
        archive = self.cache / "native-xr-release.tar.gz"
        self.assertTrue(archive.is_file())
        shutil.rmtree(self.native)
        self.run_helper("restore", expected=0)
        self.assertTrue((self.native / REQUIRED_FILES[2]).is_file())
        self.assertFalse((self.native / "godot-cpp/build/generated.o").exists())
        self.assertFalse((self.native / "godot-cpp/build/generated.os").exists())

    def test_cold_ensure_invokes_release_only_bootstrap(self) -> None:
        args_file = Path(self.temporary.name) / "bootstrap-args.txt"
        self.environment["NIGHTFALL_BOOTSTRAP_ARGS"] = str(args_file)
        self.run_helper("ensure", expected=77)
        self.assertEqual(args_file.read_text(encoding="utf-8").splitlines(), ["--release-only"])


if __name__ == "__main__":
    unittest.main()
