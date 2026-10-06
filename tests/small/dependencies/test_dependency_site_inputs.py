# SPDX-License-Identifier: GPL-2.0-only
"""Host component checks for exact site wheel selection and saved selections."""

from __future__ import annotations

import io
import json
import tempfile
from dataclasses import asdict, replace
from pathlib import Path
from typing import TYPE_CHECKING, Any
from unittest.mock import patch

import pytest
from fplinux_cli.dependencies.inputs import DependencyInput
from fplinux_cli.dependencies.site_inputs import (
    remember_site_inputs,
    resolve_site_inputs,
    site_dependency_context,
)

if TYPE_CHECKING:
    from collections.abc import Iterator
    from urllib.request import Request


class SiteDependencyInputTests:
    """Protect pinned selection, supported compatibility and offline restoration."""

    @pytest.fixture(autouse=True)
    def prepare_checkout(self) -> Iterator[None]:
        """Prepare a synthetic checkout with one literal documentation dependency."""
        temporary = tempfile.TemporaryDirectory()
        with temporary:
            self.root = Path(temporary.name)
            (self.root / "site").mkdir()
            (self.root / ".github/workflows").mkdir(parents=True)
            self.requirements = self.root / "site/requirements.txt"
            self.requirements.write_text(f"example==1.0 --hash=sha256:{'a' * 64}\n")
            self.workflow = self.root / ".github/workflows/pages.yml"
            self.workflow.write_text(
                "jobs:\n"
                "  build:\n"
                "    runs-on: ubuntu-24.04\n"
                "    steps:\n"
                "      - uses: actions/setup-python@fixture\n"
                "        with:\n"
                '          python-version: "3.14"\n'
                "  deploy:\n"
                "    runs-on: ubuntu-24.04\n"
            )
            yield

    @staticmethod
    def _input(filename: str = "example-1.0-py3-none-any.whl") -> DependencyInput:
        """Construct a manifest declaration independently of wheel resolution."""
        return DependencyInput(
            key=f"site:example:1.0:{filename}",
            url=f"https://files.pythonhosted.org/packages/original/{filename}",
            sha256="a" * 64,
            size=17,
            destination=f"downloads/site/{filename}",
            purpose="site-python",
            arch="x86_64",
        )

    def _selection(self, path: Path, item: DependencyInput) -> None:
        """Write an ordinary receipt for a caller-selected source directory."""
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"context": site_dependency_context(self.root), "inputs": [asdict(item)]})
        )

    @staticmethod
    def _metadata(filename: str, *, digest: str = "a" * 64) -> dict[str, Any]:
        """Supply published metadata at the external HTTP boundary; no wheel is installed."""
        return {
            "filename": filename,
            "url": f"https://files.pythonhosted.org/packages/original/{filename}",
            "digests": {"sha256": digest},
            "size": 17,
            "packagetype": "bdist_wheel",
            "yanked": False,
            "requires_python": ">=3.9",
        }

    def test_context_ignores_comments_pin_order_and_hash_order(self) -> None:
        """Formatting changes preserve the manifest's semantic selection context."""
        self.requirements.write_text(
            f"example==1.0 --hash=sha256:{'b' * 64} --hash=sha256:{'a' * 64}\n"
            f"other==2.0 --hash=sha256:{'c' * 64}\n"
        )
        before = site_dependency_context(self.root)
        self.requirements.write_text(
            "# Lock comment\n"
            f"other==2.0 --hash=sha256:{'c' * 64}\n"
            "example==1.0 \\\n"
            f"  --hash=sha256:{'a' * 64} \\\n"
            f"  --hash=sha256:{'b' * 64} # trailing comment\n"
        )
        self.workflow.write_text(
            "jobs:\n"
            "  build:\n"
            "    steps:\n"
            "      - with:\n"
            "          python-version: '3.14' # interpreter\n"
            "    runs-on: ubuntu-24.04 # platform\n"
        )
        assert (site_dependency_context(self.root)) == (before)
        assert (before["requirements"]) == (
            [
                {"name": "example", "version": "1.0", "hashes": ["a" * 64, "b" * 64]},
                {"name": "other", "version": "2.0", "hashes": ["c" * 64]},
            ]
        )

    def test_unpinned_requirements_and_unsupported_consumers_are_rejected(self) -> None:
        """The archive cannot silently add a latest version or another platform."""
        self.requirements.write_text(f"example>=1.0 --hash=sha256:{'a' * 64}\n")
        with pytest.raises(SystemExit, match="exact name==version"):
            site_dependency_context(self.root)
        self.requirements.write_text(f"example==1.0 --hash=sha256:{'a' * 64}\n")
        self.workflow.write_text(self.workflow.read_text().replace("3.14", "3.15"))
        with pytest.raises(SystemExit, match=r"Ubuntu 24.04 with Python 3.14"):
            site_dependency_context(self.root)

    def test_saved_selection_restores_without_network_or_cached_receipt(self) -> None:
        """A verified manifest supplies exact URLs even on a cold offline checkout."""
        item = self._input()
        with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
            actual = resolve_site_inputs(self.root, offline=True, saved_inputs=[item])
        assert (actual) == ([item])
        assert not ((self.root / ".cache").exists())

    @pytest.mark.parametrize(
        "filename",
        [
            pytest.param("example-1.0-cp314-cp314-manylinux_2_39_x86_64.whl", id="native"),
            pytest.param(
                "example-1.0-cp310-abi3-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
                id="stable-abi",
            ),
            pytest.param("example-1.0-py2.py3-none-any.whl", id="generic"),
        ],
    )
    def test_supported_native_stable_abi_and_generic_wheels_are_accepted(
        self, filename: str
    ) -> None:
        """Compatibility includes pinned native and portable CPython 3.14 wheels."""
        item = self._input(filename)
        assert resolve_site_inputs(self.root, offline=True, saved_inputs=[item]) == [item]

    @pytest.mark.parametrize(
        "filename",
        [
            pytest.param(
                "example-1.0-cp314t-cp314t-manylinux_2_17_x86_64.whl", id="free-threaded"
            ),
            pytest.param("example-1.0-cp313-cp313-manylinux_2_17_x86_64.whl", id="older-python"),
            pytest.param("example-1.0-cp314-cp314-manylinux_2_40_x86_64.whl", id="newer-libc"),
            pytest.param("example-1.0-cp314-cp314-musllinux_1_2_x86_64.whl", id="musl"),
            pytest.param("example-1.0-cp314-cp314-manylinux_2_17_aarch64.whl", id="aarch64"),
            pytest.param("example-1.0-cp314-cp314-win_amd64.whl", id="windows"),
            pytest.param("example-2.0-py3-none-any.whl", id="different-version"),
        ],
    )
    def test_incompatible_saved_wheels_are_rejected(self, filename: str) -> None:
        """Restoration rejects wheels needing another runtime, libc or architecture."""
        with pytest.raises(SystemExit, match="outside the supported selection"):
            resolve_site_inputs(self.root, offline=True, saved_inputs=[self._input(filename)])

    @pytest.mark.parametrize(
        ("changes", "message"),
        [
            pytest.param(
                {"sha256": "b" * 64},
                "does not match its exact declaration",
                id="different-hash",
            ),
            pytest.param(
                {"url": "https://example.invalid/example-1.0-py3-none-any.whl"},
                "does not match its exact declaration",
                id="different-origin",
            ),
            pytest.param(
                {"destination": "downloads/other/example-1.0-py3-none-any.whl"},
                "does not match its exact declaration",
                id="different-destination",
            ),
            pytest.param(
                {"size": None},
                "does not match its exact declaration",
                id="unknown-size",
            ),
            pytest.param(None, "selection is incomplete: example==1.0", id="incomplete-selection"),
        ],
    )
    def test_saved_selection_requires_exact_hash_origin_size_and_complete_closure(
        self, changes: dict[str, Any] | None, message: str
    ) -> None:
        """A snapshot cannot substitute undeclared bytes or omit a pinned dependency."""
        selected = [] if changes is None else [replace(self._input(), **changes)]
        with pytest.raises(SystemExit, match=message):
            resolve_site_inputs(self.root, offline=True, saved_inputs=selected)

    def test_explicit_selection_receipt_supports_offline_creation(self) -> None:
        """A preserved original directory supplies metadata without a live index."""
        source = self.root / "originals"
        item = self._input()
        self._selection(source / "site/selection.json", item)
        with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
            assert (resolve_site_inputs(self.root, offline=True, source_directories=[source])) == (
                [item]
            )

    def test_remembered_validated_selection_supports_offline_creation(self) -> None:
        """Restored declarations preserve their original URL and digest for reuse."""
        item = self._input()
        with pytest.raises(SystemExit, match="does not match its exact declaration"):
            remember_site_inputs(self.root, [replace(item, sha256="b" * 64)])
        assert not ((self.root / ".cache").exists())
        with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
            remember_site_inputs(self.root, [item])
            assert (resolve_site_inputs(self.root, offline=True)) == ([item])

    def test_missing_or_mismatched_cached_selection_reports_exact_pin_and_hash(self) -> None:
        """An ordinary declaration change makes a cached wheel selection a miss."""
        self._selection(self.root / ".cache/downloads/site/selection.json", self._input())
        self.requirements.write_text(f"example==2.0 --hash=sha256:{'b' * 64}\n")
        with (
            patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")),
            pytest.raises(SystemExit, match=f"example==2.0; allowed SHA-256: {'b' * 64}"),
        ):
            resolve_site_inputs(self.root, offline=True)

    def test_online_selection_keeps_only_compatible_hash_listed_original(self) -> None:
        """Published exact-version metadata determines the original URL and file size."""
        filename = "example-1.0-cp314-cp314-manylinux_2_17_x86_64.whl"
        metadata = {
            "urls": [
                self._metadata("example-1.0-cp314-cp314-win_amd64.whl"),
                self._metadata("example-1.0-py3-none-any.whl"),
                self._metadata(
                    "example-1.0-cp314-cp314-manylinux_2_39_x86_64.whl", digest="b" * 64
                ),
                self._metadata(filename),
            ]
        }
        requests: list[str] = []

        def published_response(request: Request, *, timeout: int) -> io.BytesIO:
            """Replace only the remote JSON response with controlled published records."""
            requests.append(request.full_url)
            assert (timeout) > (0)
            return io.BytesIO(json.dumps(metadata).encode())

        with patch("urllib.request.urlopen", side_effect=published_response):
            actual = resolve_site_inputs(self.root, offline=False)
        assert (actual) == ([self._input(filename)])
        assert (requests) == (["https://pypi.org/pypi/example/1.0/json"])
        receipt = json.loads((self.root / ".cache/downloads/site/selection.json").read_text())
        assert (receipt["inputs"]) == ([asdict(self._input(filename))])
        with patch("urllib.request.urlopen", side_effect=AssertionError("unexpected network")):
            assert (resolve_site_inputs(self.root, offline=True)) == (actual)

    def test_online_selection_rejects_a_wheel_requiring_a_newer_python(self) -> None:
        """A matching filename cannot override its published interpreter requirement."""
        wheel = self._metadata("example-1.0-py3-none-any.whl")
        wheel["requires_python"] = ">=3.15"
        response = io.BytesIO(json.dumps({"urls": [wheel]}).encode())
        with (
            patch("urllib.request.urlopen", return_value=response),
            pytest.raises(SystemExit, match=r"no supported documentation wheel for example==1.0"),
        ):
            resolve_site_inputs(self.root, offline=False)
        assert not ((self.root / ".cache/downloads/site/selection.json").exists())
