# SPDX-License-Identifier: GPL-2.0-only
"""Field ordering tests at the YAML source-text boundary."""

from __future__ import annotations

from pathlib import Path

import pytest
from fplinux_cli.quality.formatting.canonical_yaml import normalize_yaml

FIXTURES = Path(__file__).parents[3] / "fixtures/formatting/yaml"


class CanonicalYamlTests:
    """Preserve values and consumer order while rearranging known fields."""

    @pytest.mark.parametrize(
        ("case_name", "logical_path"),
        [
            pytest.param(
                "binding-literals",
                "platforms/demo/linux/overlay/Documentation/devicetree/bindings/demo.yaml",
                id="binding-literals",
            ),
            pytest.param("workflow-order", ".github/workflows/demo.yml", id="workflow-order"),
            pytest.param(
                "issue-form-order", ".github/ISSUE_TEMPLATE/report.yml", id="issue-form-order"
            ),
            pytest.param(
                "missing-fields", ".github/ISSUE_TEMPLATE/report.yml", id="missing-fields"
            ),
            pytest.param(
                "template-chomping",
                "platforms/demo/linux/overlay/Documentation/devicetree/bindings/demo.yaml.in",
                id="template-chomping",
            ),
        ],
    )
    def test_canonical_bytes_preserve_values_and_order(
        self, case_name: str, logical_path: str
    ) -> None:
        """Keep exact scalar, comment and sequence bytes while ordering known fields."""
        case = FIXTURES / case_name
        source = (case / "input.yaml.fixture").read_bytes()
        expected = (case / "expected.yaml.fixture").read_bytes()

        actual = normalize_yaml(logical_path, source)

        assert actual == expected
        assert normalize_yaml(logical_path, actual) == actual

    @pytest.mark.parametrize(
        "source",
        [
            b"name: [\n",
            b"name: first\nname: second\n",
            b"env: &vars {NAME: demo}\njobs: {build: {env: *vars}}\n",
        ],
        ids=["invalid-syntax", "duplicate-key", "aliased-document"],
    )
    def test_invalid_duplicate_and_aliased_documents_are_rejected(self, source: bytes) -> None:
        """Fail rather than discard syntax errors or reorder alias providers."""
        with pytest.raises(ValueError):  # noqa: PT011 -- stable rejection type; diagnostics vary.
            normalize_yaml(".github/workflows/demo.yml", source)

    def test_shuffled_flow_mapping_requires_a_block_mapping(self) -> None:
        """Avoid rewriting inline YAML without preserving its comment boundaries."""
        with pytest.raises(ValueError, match="block YAML mapping"):
            normalize_yaml(".github/ISSUE_TEMPLATE/report.yml", b"{body: [], name: Demo}\n")
