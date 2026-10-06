# SPDX-License-Identifier: GPL-2.0-only
"""Public build diagnostics for syntax errors in editable manifests."""

from __future__ import annotations

import shutil
from pathlib import Path

from tests.process import run_process

ROOT = Path(__file__).resolve().parents[2]


class ManifestErrorWorkflowTests:
    """Run the source CLI in an isolated checkout before any build backend."""

    def test_invalid_release_toml_reports_the_file_without_a_traceback(
        self, tmp_path: Path
    ) -> None:
        """A contributor gets the filename and syntax location, not a Python stack."""
        checkout = tmp_path
        shutil.copy2(ROOT / "fplinux", checkout / "fplinux")
        for name in (
            "scripts",
            "targets",
            "platforms",
            "profiles",
            "alpine",
            "bootstrap",
            "common",
            "include",
            "lib",
        ):
            shutil.copytree(
                ROOT / name,
                checkout / name,
                ignore=shutil.ignore_patterns("__pycache__"),
            )
        for path in ROOT.glob("*.toml"):
            shutil.copy2(path, checkout / path.name)
        for name in ("Containerfile", "THIRD_PARTY_NOTICES.md"):
            shutil.copy2(ROOT / name, checkout / name)
        target = checkout / "targets/nokia-ta1618"
        (target / "release/manifest.toml").write_text("image = [\n", encoding="utf-8")
        result = run_process(
            [str(checkout / "fplinux"), "build", "nokia-ta1618", "--offline"],
            name="public build with invalid release TOML",
            timeout=10,
            cwd=checkout,
        )

        assert (result.returncode) == (1), result.stderr
        assert (result.stdout) == ("")
        assert ("fplinux:") in (result.stderr)
        assert ("targets/nokia-ta1618/release/manifest.toml") in (result.stderr)
        assert ("Invalid value") in (result.stderr)
        assert ("Traceback") not in (result.stderr)
