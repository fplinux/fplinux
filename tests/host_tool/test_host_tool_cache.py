# SPDX-License-Identifier: GPL-2.0-only
"""Exercise host-tool reuse with a real native compile and disposable inputs."""

from __future__ import annotations

import hashlib
import os
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from fplinux_cli import common
from fplinux_cli.build import host as host_build
from fplinux_cli.build import inputs as inputs_build
from fplinux_cli.build import process as process_build
from fplinux_cli.build import sources as sources_build


class HostToolCacheTests(unittest.TestCase):
    """A selected input change recompiles; an unrelated source change does not."""

    def test_selected_inputs_image_and_compiler_control_reuse(self) -> None:
        """Reuse a verified executable until one of its actual build inputs changes."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "cache"
            archive = root / "bridge.tar.gz"
            upstream = root / "upstream"
            upstream.mkdir()
            makefile = upstream / "Makefile"
            makefile.write_text(
                "clean:\n\trm -f bridge\nall:\n\t$(CC) -nostdlib -static -o bridge bridge.S\n",
                encoding="ascii",
            )
            source = upstream / "bridge.S"
            source.write_text(
                '#include "local-input.h"\n.global _start\n_start:\n'
                "    mov $1, %rax\n    mov $1, %rdi\n"
                "    lea message(%rip), %rsi\n    mov $(message_end-message), %rdx\n"
                "    syscall\n    mov $60, %rax\n    xor %rdi, %rdi\n    syscall\n"
                '.section .rodata\nmessage:\n    .ascii "base:"\n'
                "    .ascii MESSAGE\n    .byte 10\nmessage_end:\n",
                encoding="ascii",
            )
            local_header = root / "platforms/demo/local-input.h"
            local_header.parent.mkdir(parents=True)
            local_header.write_text('#define MESSAGE "one"\n', encoding="ascii")
            patch = root / "platforms/demo/local.patch"

            def write_patch(prefix: str) -> None:
                patch.write_text(
                    "--- a/bridge.S\n+++ b/bridge.S\n@@ -14 +14 @@\n"
                    '-    .ascii "base:"\n+    .ascii "' + prefix + ':"\n',
                    encoding="ascii",
                )

            def write_archive() -> None:
                with tarfile.open(archive, "w:gz") as tar:
                    for name in ("Makefile", "bridge.S"):
                        tar.add(upstream / name, arcname=f"bridge-deadbeef/{name}")

            def sources() -> dict[str, object]:
                return {
                    "bridge-source": {
                        "archive_url": "https://example.invalid/bridge.tar.gz",
                        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                        "commit": "deadbeef",
                        "files": {
                            "makefile_sha256": hashlib.sha256(makefile.read_bytes()).hexdigest(),
                            "bridge_s_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        },
                    }
                }

            write_patch("A")
            write_archive()
            recipe = {
                "type": "make-archive",
                "name": "bridge",
                "source_lock": "bridge-source",
                "cache_name": "bridge.tar.gz",
                "archive_prefix": "bridge-{commit}/",
                "source_directory": "bridge",
                "binary": "bridge",
                "link": "static-libusb",
                "members": [
                    {"path": "Makefile", "digest_key": "makefile_sha256"},
                    {"path": "bridge.S", "digest_key": "bridge_s_sha256"},
                ],
                "copies": [
                    {"source": "platforms/demo/local-input.h", "destination": "local-input.h"}
                ],
                "patches": ["platforms/demo/local.patch"],
                "self_test": True,
            }
            platform = {"host": {"tools": [recipe]}}
            original_run = process_build.run
            calls = {"make": 0, "self_test": 0}

            def run(
                command: list[str],
                *,
                cwd: Path | None = None,
                environment: dict[str, str] | None = None,
            ) -> None:
                if command[0] == "make":
                    calls["make"] += 1
                if command[-1] == "--self-test":
                    calls["self_test"] += 1
                original_run(command, cwd=cwd, environment=environment)

            build_number = 0

            def build(expected: str, expected_compiles: int) -> None:
                nonlocal build_number
                build_number += 1
                result = host_build.build_host_tools(
                    sources(), platform, root / f"work-{build_number}"
                )
                binary = result["bridge"]
                self.assertEqual(
                    subprocess.check_output([str(binary)], text=True), expected + "\n"
                )
                self.assertEqual(calls["make"], expected_compiles)

            environment = {
                "FPLINUX_CONTAINER_IMAGE_SOURCE_RECIPE": "a" * 64,
                "FPLINUX_CONTAINER_IMAGE_GENERATION": "b" * 64,
                "PATH": os.environ.get("PATH", os.defpath),
            }
            with (
                mock.patch.object(common, "ROOT", root),
                mock.patch.object(inputs_build, "CACHE", cache),
                mock.patch.object(sources_build, "fetch", return_value=archive),
                mock.patch.object(process_build, "run", side_effect=run),
                mock.patch.dict(os.environ, environment, clear=True),
            ):
                build("A:one", 1)
                build("A:one", 1)
                (root / "unrelated-driver.c").write_text("changed\n", encoding="ascii")
                build("A:one", 1)

                local_header.write_text('#define MESSAGE "two"\n', encoding="ascii")
                build("A:two", 2)
                write_patch("B")
                build("B:two", 3)
                makefile.write_text(makefile.read_text(encoding="ascii") + "# changed\n")
                write_archive()
                build("B:two", 4)

                with mock.patch.dict(os.environ, {"CC": "cc"}):
                    build("B:two", 5)
                    with mock.patch.dict(
                        os.environ, {"FPLINUX_CONTAINER_IMAGE_GENERATION": "c" * 64}
                    ):
                        build("B:two", 6)
                        self.assertEqual(calls["self_test"], 8)
                        receipt = (cache / "host-tools/bridge/receipt.json").read_bytes()
                        local_header.write_text("#error broken local input\n", encoding="ascii")
                        with self.assertRaises(subprocess.CalledProcessError):
                            build("", 7)
                        self.assertEqual(
                            (cache / "host-tools/bridge/receipt.json").read_bytes(), receipt
                        )
                        local_header.write_text('#define MESSAGE "two"\n', encoding="ascii")
                        build("B:two", 7)


if __name__ == "__main__":
    unittest.main()
