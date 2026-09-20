# SPDX-License-Identifier: GPL-2.0-only
"""Host-process coverage for the MicroPythonOS shell storage policy.

The shipped policy library is sourced normally and run against temporary paths
and stub ``mount``/``umount`` programs. These tests verify command selection,
ownership and exported environment; they do not validate MMC, VFAT, write-back
or card persistence.
"""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from tests.process import run_process

if TYPE_CHECKING:
    import subprocess

ROOT = Path(__file__).resolve().parents[2]
BASE_APORT = ROOT / "alpine/aports/fplinux-micropythonos"
WRAPPER_POLICY = BASE_APORT / "micropythonos-wrapper.sh"
STORAGE_POLICY_FIXTURES = ROOT / "tests/fixtures/storage_policy"
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
    configured_state_file: str | None
    fallback_state_file: str | None


@dataclass(frozen=True)
class StoragePolicyFixture:
    """Paths that define one controlled storage-policy process boundary."""

    root: Path
    configuration: Path
    card: Path
    partition_device: Path
    whole_device: Path
    fallback_root: Path
    runtime: Path
    launcher: Path
    commands: Path
    events: Path
    runtime_environment: Path
    mountinfo: Path
    driver: Path


class MicroPythonOsStoragePolicyTests(unittest.TestCase):
    """Exercise policy decisions through a bounded shell process."""

    def run_storage_policy(
        self,
        *,
        device_nodes_present: bool = True,
        mountinfo_filesystem: str | None = None,
        root_mountinfo_entry: str = "1 1 0:2 / / rw - rootfs rootfs rw\n",
    ) -> StoragePolicyRun:
        """Run the shipped policy through a driver that supplies controlled paths."""
        with tempfile.TemporaryDirectory() as temporary:
            fixture = self.prepare_storage_policy_fixture(
                Path(temporary),
                device_nodes_present=device_nodes_present,
                mountinfo_filesystem=mountinfo_filesystem,
                root_mountinfo_entry=root_mountinfo_entry,
            )
            result = run_process(
                [str(fixture.driver)],
                name="MicroPythonOS storage policy",
                timeout=_POLICY_TIMEOUT_SECONDS,
                cwd=fixture.root,
                env=self.storage_policy_environment(fixture),
            )
            return self.collect_storage_policy_run(fixture, result)

    @staticmethod
    def prepare_storage_policy_fixture(
        root: Path,
        *,
        device_nodes_present: bool,
        mountinfo_filesystem: str | None,
        root_mountinfo_entry: str,
    ) -> StoragePolicyFixture:
        """Create the controlled files and command doubles for one policy run."""
        fixture = StoragePolicyFixture(
            root=root,
            configuration=root / "etc/fplinux/micropythonos.conf",
            card=root / "mnt/card",
            partition_device=root / "dev/mmcblk0p1",
            whole_device=root / "dev/mmcblk0",
            fallback_root=root / "var/lib/micropythonos",
            runtime=root / "runtime",
            launcher=root / "launcher",
            commands=root / "bin",
            events=root / "events",
            runtime_environment=root / "runtime-environment",
            mountinfo=root / "mountinfo",
            driver=root / "run-storage-policy",
        )
        fixture.configuration.parent.mkdir(parents=True)
        fixture.configuration.write_text(
            "\n".join(
                (
                    f"MPOS_STORAGE={fixture.card}",
                    f'MPOS_STORAGE_DEVICES="{fixture.partition_device} {fixture.whole_device}"',
                    "MPOS_STORAGE_FSTYPE=vfat",
                    "MPOS_STORAGE_STATE_DIR=.fplinux/micropythonos",
                    f"MPOS_ROOT={fixture.fallback_root}",
                    "MPOS_HEAP_SIZE=4194304",
                )
            )
            + "\n",
            encoding="utf-8",
        )
        mounted_card = ""
        if mountinfo_filesystem is not None:
            mounted_card = (
                f"36 25 179:1 / {fixture.card} rw,relatime - "
                f"{mountinfo_filesystem} {fixture.partition_device} rw\n"
            )
        fixture.mountinfo.write_text(root_mountinfo_entry + mounted_card, encoding="utf-8")
        if device_nodes_present:
            fixture.partition_device.parent.mkdir(parents=True)
            fixture.partition_device.touch()
            fixture.whole_device.touch()

        fixture.commands.mkdir()
        programs = {
            fixture.runtime: "runtime",
            fixture.launcher: "launcher",
            fixture.commands / "mount": "mount",
            fixture.commands / "umount": "umount",
            fixture.driver: "run-storage-policy",
        }
        for destination, name in programs.items():
            shutil.copyfile(STORAGE_POLICY_FIXTURES / name, destination)
            destination.chmod(0o755)
        return fixture

    @staticmethod
    def storage_policy_environment(fixture: StoragePolicyFixture) -> dict[str, str]:
        """Describe the host boundaries visible to the policy and its command doubles."""
        return os.environ | {
            "PATH": f"{fixture.commands}:{os.environ['PATH']}",
            "FPLINUX_TEST_EVENTS": str(fixture.events),
            "FPLINUX_TEST_PARTITION": str(fixture.partition_device),
            "FPLINUX_TEST_CARD": str(fixture.card),
            "FPLINUX_TEST_CONFIG": str(fixture.configuration),
            "FPLINUX_TEST_LAUNCHER": str(fixture.launcher),
            "FPLINUX_TEST_MOUNTINFO": str(fixture.mountinfo),
            "FPLINUX_TEST_POLICY": str(WRAPPER_POLICY),
            "FPLINUX_TEST_RUNTIME": str(fixture.runtime),
            "FPLINUX_TEST_RUNTIME_ENVIRONMENT": str(fixture.runtime_environment),
        }

    @classmethod
    def collect_storage_policy_run(
        cls,
        fixture: StoragePolicyFixture,
        result: subprocess.CompletedProcess[str],
    ) -> StoragePolicyRun:
        """Collect observable policy results before the temporary tree is removed."""
        runtime_values: dict[str, str] = {}
        runtime_arguments: list[str] = []
        for line in fixture.runtime_environment.read_text(encoding="utf-8").splitlines():
            key, _, value = line.partition("=")
            if key == "argv":
                runtime_arguments.append(value)
            else:
                runtime_values[key] = value
        return StoragePolicyRun(
            result_returncode=result.returncode,
            result_stderr=result.stderr,
            fixture_root=fixture.root,
            configured_storage_path=fixture.card,
            fallback_root=fixture.fallback_root,
            partition_device_path=fixture.partition_device,
            whole_device_path=fixture.whole_device,
            command_events=(
                tuple(fixture.events.read_text(encoding="utf-8").splitlines())
                if fixture.events.exists()
                else ()
            ),
            runtime_environment=runtime_values,
            runtime_arguments=tuple(runtime_arguments),
            runtime_working_directory=runtime_values["cwd"],
            configured_state_directory_created=(
                fixture.card / ".fplinux/micropythonos/prefs"
            ).is_dir(),
            fallback_state_directory_created=(fixture.fallback_root / "prefs").is_dir(),
            configured_state_file=cls.read_optional_text(
                fixture.card / ".fplinux/micropythonos/data/storage-probe.txt"
            ),
            fallback_state_file=cls.read_optional_text(
                fixture.fallback_root / "data/storage-probe.txt"
            ),
        )

    @staticmethod
    def read_optional_text(path: Path) -> str | None:
        """Read the state file if the runtime wrote it at this location."""
        return path.read_text(encoding="utf-8") if path.exists() else None

    def test_policy_never_mounts_a_declared_device_it_finds_unmounted(self) -> None:
        """A present declared card that nobody mounted stays untouched."""
        run = self.run_storage_policy()

        self.assertEqual(run.result_returncode, 0, run.result_stderr)
        self.assertEqual(run.command_events, ())
        self.assertEqual(run.runtime_environment["MPOS_STORAGE"], "")
        self.assertEqual(run.runtime_environment["MPOS_ROOT"], str(run.fallback_root))
        self.assertEqual(
            run.runtime_arguments,
            (
                "-X",
                "heapsize=4194304",
                "-c",
                'from mpos import DeviceInfo; DeviceInfo.set_hardware_id("fplinux"); import main',
            ),
        )
        self.assertEqual(run.runtime_working_directory, str(run.fallback_root))
        self.assertFalse(run.configured_state_directory_created)
        self.assertTrue(run.fallback_state_directory_created)

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

    def test_policy_writes_application_state_on_the_system_root_in_both_root_modes(self) -> None:
        """Without a usable card the writes land on whichever system root is active."""
        for root_entry in (
            "1 1 0:2 / / rw - rootfs rootfs rw\n",
            "16 1 179:2 / / rw,relatime - ext4 /dev/root rw\n",
        ):
            with self.subTest(root_mountinfo=root_entry):
                run = self.run_storage_policy(root_mountinfo_entry=root_entry)

                self.assertEqual(run.result_returncode, 0, run.result_stderr)
                self.assertEqual(run.runtime_environment["MPOS_ROOT"], str(run.fallback_root))
                self.assertEqual(run.runtime_working_directory, str(run.fallback_root))
                self.assertEqual(run.runtime_environment["MPOS_STORAGE"], "")
                self.assertEqual(run.command_events, ())
                self.assertEqual(run.fallback_state_file, "application state\n")
                self.assertIsNone(run.configured_state_file)
                self.assertFalse(run.configured_state_directory_created)

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
