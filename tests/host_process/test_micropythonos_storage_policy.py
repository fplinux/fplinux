# SPDX-License-Identifier: GPL-2.0-only
"""Host-process coverage for the MicroPythonOS shell storage policy.

The shipped policy library is sourced normally and run against temporary paths
and stub ``mount``/``umount`` programs. These tests verify command selection,
ownership and exported environment; they do not validate MMC, VFAT, write-back
or card persistence.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]
BASE_APORT = ROOT / "alpine/aports/fplinux-micropythonos"
WRAPPER_POLICY = BASE_APORT / "micropythonos-wrapper.sh"
_POLICY_TIMEOUT_SECONDS = 10


@dataclass(frozen=True)
class StoragePolicyRun:
    """Observations from one policy run with temporary command doubles."""

    result_returncode: int
    result_stderr: str
    fixture_root: Path
    configured_storage_path: Path
    fallback_root: Path
    partition_device_path: Path
    whole_device_path: Path
    command_events: tuple[str, ...]
    runtime_environment: dict[str, str]
    runtime_arguments: tuple[str, ...]
    runtime_working_directory: str
    configured_state_directory_created: bool
    fallback_state_directory_created: bool


class MicroPythonOsStoragePolicyTests(unittest.TestCase):
    """Exercise policy decisions through a bounded shell process."""

    def run_storage_policy(
        self,
        *,
        device_nodes_present: bool = True,
        reject_partition: bool = False,
        mountinfo_filesystem: str | None = None,
    ) -> StoragePolicyRun:
        """Run the shipped policy through a driver that supplies controlled paths."""
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            configuration = root / "etc/fplinux/micropythonos.conf"
            card = root / "mnt/card"
            device_one = root / "dev/mmcblk0p1"
            device_two = root / "dev/mmcblk0"
            volatile_root = root / "volatile"
            runtime = root / "runtime"
            launcher = root / "launcher"
            commands = root / "bin"
            events = root / "events"
            runtime_environment = root / "runtime-environment"
            mountinfo_path = root / "mountinfo"

            configuration.parent.mkdir(parents=True)
            configuration.write_text(
                "\n".join(
                    (
                        f"MPOS_STORAGE={card}",
                        f'MPOS_STORAGE_DEVICES="{device_one} {device_two}"',
                        "MPOS_STORAGE_FSTYPE=vfat",
                        "MPOS_STORAGE_STATE_DIR=.fplinux/micropythonos",
                        f"MPOS_ROOT={volatile_root}",
                        "MPOS_HEAP_SIZE=4194304",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            mountinfo_path.write_text(
                (
                    f"36 25 179:1 / {card} rw,relatime - {mountinfo_filesystem} {device_one} rw\n"
                    if mountinfo_filesystem is not None
                    else ""
                ),
                encoding="utf-8",
            )
            if device_nodes_present:
                device_one.parent.mkdir(parents=True)
                device_one.touch()
                device_two.touch()

            self.write_executable(
                runtime,
                """#!/bin/sh
{
        printf 'cwd=%s\\n' "$PWD"
        printf 'MPOS_STORAGE=%s\\n' "$MPOS_STORAGE"
        printf 'MPOS_ROOT=%s\\n' "$MPOS_ROOT"
        printf 'MPOS_PACKAGED_APPS=%s\\n' "$MPOS_PACKAGED_APPS"
        for argument in "$@"; do
                printf 'argv=%s\\n' "$argument"
        done
} > "$FPLINUX_TEST_RUNTIME_ENVIRONMENT"
""",
            )
            self.write_executable(launcher, '#!/bin/sh\nexec "$@"\n')
            commands.mkdir()
            self.write_executable(
                commands / "mount",
                """#!/bin/sh
printf 'mount' >> "$FPLINUX_TEST_EVENTS"
for argument in "$@"; do
        printf '\\t%s' "$argument" >> "$FPLINUX_TEST_EVENTS"
done
printf '\\tcwd=%s\\n' "$PWD" >> "$FPLINUX_TEST_EVENTS"
[ "$3" = "$FPLINUX_TEST_REJECT_DEVICE" ] && exit 1
exit 0
""",
            )
            self.write_executable(
                commands / "umount",
                """#!/bin/sh
printf 'umount' >> "$FPLINUX_TEST_EVENTS"
for argument in "$@"; do
        printf '\\t%s' "$argument" >> "$FPLINUX_TEST_EVENTS"
done
printf '\\tcwd=%s\\n' "$PWD" >> "$FPLINUX_TEST_EVENTS"
case "$PWD/" in
"$FPLINUX_TEST_CARD"/*) exit 32 ;;
esac
exit 0
""",
            )

            driver = root / "run-storage-policy"
            self.write_executable(
                driver,
                """#!/bin/sh
set -eu
set -f
. "$FPLINUX_TEST_POLICY"
mpos_config_path=$FPLINUX_TEST_CONFIG
mpos_framebuffer_path=/dev/null
mpos_input_path=/dev
mpos_mountinfo_path=$FPLINUX_TEST_MOUNTINFO
mpos_runtime_path=$FPLINUX_TEST_RUNTIME
mpos_launcher_path=$FPLINUX_TEST_LAUNCHER
micropythonos_main "$@"
""",
            )

            environment = os.environ | {
                "PATH": f"{commands}:{os.environ['PATH']}",
                "FPLINUX_TEST_EVENTS": str(events),
                "FPLINUX_TEST_REJECT_DEVICE": str(device_one) if reject_partition else "",
                "FPLINUX_TEST_CARD": str(card),
                "FPLINUX_TEST_CONFIG": str(configuration),
                "FPLINUX_TEST_LAUNCHER": str(launcher),
                "FPLINUX_TEST_MOUNTINFO": str(mountinfo_path),
                "FPLINUX_TEST_POLICY": str(WRAPPER_POLICY),
                "FPLINUX_TEST_RUNTIME": str(runtime),
                "FPLINUX_TEST_RUNTIME_ENVIRONMENT": str(runtime_environment),
            }
            result = run_process(
                [str(driver)],
                name="MicroPythonOS storage policy",
                timeout=_POLICY_TIMEOUT_SECONDS,
                cwd=root,
                env=environment,
            )
            runtime_lines = runtime_environment.read_text(encoding="utf-8").splitlines()
            runtime_values: dict[str, str] = {}
            runtime_arguments: list[str] = []
            for line in runtime_lines:
                key, _, value = line.partition("=")
                if key == "argv":
                    runtime_arguments.append(value)
                else:
                    runtime_values[key] = value
            return StoragePolicyRun(
                result_returncode=result.returncode,
                result_stderr=result.stderr,
                fixture_root=root,
                configured_storage_path=card,
                fallback_root=volatile_root,
                partition_device_path=device_one,
                whole_device_path=device_two,
                command_events=tuple(events.read_text(encoding="utf-8").splitlines())
                if events.exists()
                else (),
                runtime_environment=runtime_values,
                runtime_arguments=tuple(runtime_arguments),
                runtime_working_directory=runtime_values["cwd"],
                configured_state_directory_created=(
                    card / ".fplinux/micropythonos/prefs"
                ).is_dir(),
                fallback_state_directory_created=(volatile_root / "prefs").is_dir(),
            )

    @staticmethod
    def write_executable(path: Path, contents: str) -> None:
        """Write a small shell fixture with executable permissions."""
        path.write_text(contents, encoding="utf-8")
        path.chmod(0o755)

    def test_policy_mounts_configured_device_runs_runtime_and_unmounts_owned_path(self) -> None:
        """A successful mount command selects the configured state path for this run."""
        run = self.run_storage_policy()

        self.assertEqual(run.result_returncode, 0, run.result_stderr)
        card = str(run.configured_storage_path)
        self.assertEqual(
            run.command_events,
            (
                f"mount\t-t\tvfat\t{run.partition_device_path}\t{card}\tcwd={run.fixture_root}",
                f"umount\t{card}\tcwd=/",
            ),
        )
        self.assertEqual(run.runtime_environment["MPOS_ROOT"], f"{card}/.fplinux/micropythonos")
        self.assertEqual(
            run.runtime_arguments,
            (
                "-X",
                "heapsize=4194304",
                "-c",
                'from mpos import DeviceInfo; DeviceInfo.set_hardware_id("fplinux"); import main',
            ),
        )
        self.assertEqual(run.runtime_working_directory, run.runtime_environment["MPOS_ROOT"])
        self.assertTrue(run.configured_state_directory_created)
        self.assertFalse(run.fallback_state_directory_created)

    def test_policy_tries_whole_device_only_after_partition_command_fails(self) -> None:
        """The wrapper preserves the configured device order around a failed command."""
        run = self.run_storage_policy(reject_partition=True)

        self.assertEqual(run.result_returncode, 0, run.result_stderr)
        card = str(run.configured_storage_path)
        self.assertEqual(
            run.command_events,
            (
                f"mount\t-t\tvfat\t{run.partition_device_path}\t{card}\tcwd={run.fixture_root}",
                f"mount\t-t\tvfat\t{run.whole_device_path}\t{card}\tcwd={run.fixture_root}",
                f"umount\t{card}\tcwd=/",
            ),
        )
        self.assertTrue(run.configured_state_directory_created)

    def test_policy_uses_fallback_root_when_device_paths_are_absent(self) -> None:
        """Without declared device nodes no mount command is issued or advertised."""
        run = self.run_storage_policy(device_nodes_present=False)

        self.assertEqual(run.result_returncode, 0, run.result_stderr)
        self.assertEqual(run.command_events, ())
        self.assertEqual(run.runtime_environment["MPOS_STORAGE"], "")
        self.assertEqual(run.runtime_environment["MPOS_ROOT"], str(run.fallback_root))
        self.assertEqual(run.runtime_working_directory, run.runtime_environment["MPOS_ROOT"])
        self.assertFalse(run.configured_state_directory_created)
        self.assertTrue(run.fallback_state_directory_created)

    def test_policy_does_not_unmount_a_matching_mountinfo_declaration(self) -> None:
        """A matching declared mount is used without invoking either command stub."""
        run = self.run_storage_policy(mountinfo_filesystem="vfat")

        self.assertEqual(run.result_returncode, 0, run.result_stderr)
        self.assertEqual(run.command_events, ())
        self.assertEqual(run.runtime_environment["MPOS_STORAGE"], str(run.configured_storage_path))
        self.assertEqual(
            run.runtime_environment["MPOS_ROOT"],
            f"{run.configured_storage_path}/.fplinux/micropythonos",
        )
        self.assertTrue(run.configured_state_directory_created)
        self.assertFalse(run.fallback_state_directory_created)

    def test_policy_rejects_a_matching_path_with_wrong_filesystem_type(self) -> None:
        """A mountinfo path alone is insufficient when it violates the declared contract."""
        run = self.run_storage_policy(mountinfo_filesystem="ext4")

        self.assertEqual(run.result_returncode, 0, run.result_stderr)
        self.assertEqual(run.command_events, ())
        self.assertEqual(run.runtime_environment["MPOS_STORAGE"], "")
        self.assertEqual(run.runtime_environment["MPOS_ROOT"], str(run.fallback_root))
        self.assertFalse(run.configured_state_directory_created)
        self.assertTrue(run.fallback_state_directory_created)


if __name__ == "__main__":
    unittest.main()
