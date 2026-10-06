# SPDX-License-Identifier: GPL-2.0-only
"""Shared source-cache access across a real subordinate user namespace."""

from __future__ import annotations

import os
import pwd
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli.alpine import aports as alpine_builder


def _build_fixture(directory: Path, *, fail_build: bool) -> None:
    """Stub Alpine tools and account lookup, retaining real cache ownership and APKs."""
    os.umask(0o022)
    directory.chmod(0o755)
    cache = directory / "cache"
    sources = cache / "downloads/alpine/sources"
    sources.mkdir(parents=True, mode=0o700)
    existing = sources / "existing.tar.xz"
    existing.write_bytes(b"existing source\n")
    existing.chmod(0o600)
    key = directory / "fixture.rsa"
    public_key = directory / "fixture.rsa.pub"
    key.write_bytes(b"fixture private key\n")
    public_key.write_bytes(b"fixture public key\n")
    lookup = pwd.getpwnam

    def account(user: str) -> pwd.struct_passwd:
        if user == "builder":
            return pwd.struct_passwd((user, "x", 1000, 1000, "", str(directory), "/bin/sh"))
        return lookup(user)

    build_count = 0

    def run_tool(command: list[str], *, cwd: Path, environment: dict[str, str]) -> None:
        nonlocal build_count
        if command[0] == "apkbuild-lint":
            return
        build_count += 1
        # The child stands in for abuild: it writes private source files and an APK.
        script = (
            "import io, pathlib, sys, tarfile; "
            "sources=pathlib.Path(sys.argv[1]); sources.chmod(0o700); "
            "assert (sources/'existing.tar.xz').read_bytes()==b'existing source\\n'; "
            "payload=sources/'downloaded.tar.xz'; payload.write_bytes(b'new source\\n'); "
            "payload.chmod(0o600); repository=pathlib.Path(sys.argv[2]); "
            "metadata=b'pkgname = fplinux-xkb-ru\\n'; "
            "archive=tarfile.open(repository/'fplinux-xkb-ru-2.48-r0.apk','w:gz'); "
            "member=tarfile.TarInfo('.PKGINFO'); member.size=len(metadata); "
            "archive.addfile(member,io.BytesIO(metadata)); archive.close()"
        )
        subprocess.run(
            [sys.executable, "-c", script, environment["SRCDEST"], environment["REPODEST"]],
            cwd=cwd,
            user=1000,
            group=1000,
            check=True,
        )
        if fail_build:
            raise subprocess.CalledProcessError(7, command)

    with (
        mock.patch.object(alpine_builder, "CACHE", cache),
        mock.patch.object(pwd, "getpwnam", side_effect=account),
        mock.patch.object(alpine_builder, "_run_as_builder", side_effect=run_tool),
        mock.patch.object(
            alpine_builder, "_builder_output", return_value="fplinux-xkb-ru-2.48-r0.apk\n"
        ),
        mock.patch.dict(os.environ, {"FPLINUX_CONTAINER_IMAGE_RECIPE": "a" * 64}),
    ):
        for iteration in range(1 if fail_build else 2):
            work = directory / f"work-{iteration}"
            work.mkdir()
            try:
                alpine_builder._build_fplinux_apks(  # noqa: SLF001 -- production build owner.
                    {"triplet": "armv7-alpine-linux-musleabihf"},
                    sysroot=directory / "sysroot",
                    work=work,
                    jobs=1,
                    private_key=key,
                    public_key=public_key,
                    build_packages=("fplinux-xkb-ru",),
                )
            except subprocess.CalledProcessError as error:
                if not fail_build or error.returncode != 7:
                    raise
            else:
                if fail_build:
                    message = "the failing build was accepted"
                    raise AssertionError(message)
    if build_count != 1:
        raise AssertionError(f"expected one build followed by cache reuse, got {build_count}")


class AlpineSourceOwnershipTests(unittest.TestCase):
    """Stub build tools omit compilation; ownership and host filesystem access are real."""

    def test_host_can_reuse_sources_after_success_and_failure(self) -> None:
        """A build, a package-cache hit and a failed build leave source files reusable."""
        unshare = shutil.which("unshare")
        if unshare is None:
            self.skipTest("subordinate namespace test requires unshare")
        probe = subprocess.run(
            [unshare, "--map-auto", "--map-root-user", "--", "true"],
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        if probe.returncode:
            self.skipTest("subordinate namespace is unavailable: " + probe.stderr.strip())
        for fail_build in (False, True):
            with self.subTest(fail_build=fail_build), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                command = [
                    unshare,
                    "--map-auto",
                    "--map-root-user",
                    "--",
                    sys.executable,
                    "-m",
                    "tests.host_process.alpine.test_alpine_source_ownership",
                    "--fixture",
                    temporary,
                    str(int(fail_build)),
                ]
                try:
                    result = subprocess.run(
                        command, capture_output=True, text=True, check=False, timeout=30
                    )
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    sources = directory / "cache/downloads/alpine/sources"
                    for path in (sources, *sources.iterdir()):
                        metadata = path.stat()
                        self.assertEqual(
                            (metadata.st_uid, metadata.st_gid), (os.getuid(), os.getgid())
                        )
                        self.assertEqual(
                            stat.S_IMODE(metadata.st_mode), 0o700 if path.is_dir() else 0o600
                        )
                    self.assertEqual(
                        (sources / "existing.tar.xz").read_bytes(), b"existing source\n"
                    )
                    self.assertEqual((sources / "downloaded.tar.xz").read_bytes(), b"new source\n")
                    with tempfile.NamedTemporaryFile(dir=sources, prefix=".input-") as stream:
                        stream.write(b"restored input\n")
                finally:
                    # A failed pre-fix case may leave subordinate-owned temporary files.
                    subprocess.run(
                        [
                            unshare,
                            "--map-auto",
                            "--map-root-user",
                            "--",
                            "chown",
                            "-R",
                            "0:0",
                            temporary,
                        ],
                        check=True,
                        timeout=30,
                    )


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "--fixture":
        _build_fixture(Path(sys.argv[2]), fail_build=bool(int(sys.argv[3])))
    else:
        unittest.main()
