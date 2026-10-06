# SPDX-License-Identifier: GPL-2.0-only
"""Field ordering tests at the YAML source-text boundary."""

from __future__ import annotations

import pytest
from fplinux_cli.quality.formatting.canonical_yaml import normalize_yaml


class CanonicalYamlTests:
    """Preserve values and consumer order while rearranging known fields."""

    def test_binding_orders_fields_without_rewriting_literal_examples(self) -> None:
        """Keep scalar spelling, literal content and unknown fields unchanged."""
        source = b"""# SPDX-License-Identifier: GPL-2.0-only
%YAML 1.2
---
examples:
  - |
    chip@10 {
        reg = <0x00000010 0x10>;
    };
additionalProperties: false
required: [reg, compatible]
properties:
  reg:
    const: 0x00000010 # address
  label:
    const: "yes"
title: Demo
# Description stays with its field.
description: 'on'
$schema: https://example.test/schema
$id: https://example.test/demo
x-note: keep
"""
        expected = b"""# SPDX-License-Identifier: GPL-2.0-only
%YAML 1.2
---
$id: https://example.test/demo
$schema: https://example.test/schema
title: Demo
# Description stays with its field.
description: 'on'
properties:
  reg:
    const: 0x00000010 # address
  label:
    const: "yes"
required: [reg, compatible]
additionalProperties: false
examples:
  - |
    chip@10 {
        reg = <0x00000010 0x10>;
    };
x-note: keep
"""
        actual = normalize_yaml("platforms/demo/linux/bindings/demo.yaml", source)
        assert (actual) == (expected)
        assert (normalize_yaml("platforms/demo/linux/bindings/demo.yaml", actual)) == (actual)

    def test_workflow_retains_jobs_steps_events_and_comments(self) -> None:
        """Move fields inside each job and step without moving executions."""
        source = b"""jobs:
  second:
    steps:
      - run: echo second
        # Caption for this step.
        name: Second
      - with:
          token: "${{ secrets.TOKEN }}"
        uses: example/action@abc # pinned
        id: action
    env:
      YES: yes
    runs-on: ubuntu-latest
    needs: first
  first:
    runs-on: ubuntu-latest
    steps: []
on:
  push:
    branches: [release, main]
name: Demo
permissions: {}
"""
        expected = b"""name: Demo
on:
  push:
    branches: [release, main]
permissions: {}
jobs:
  second:
    needs: first
    runs-on: ubuntu-latest
    env:
      YES: yes
    steps:
      - # Caption for this step.
        name: Second
        run: echo second
      - id: action
        uses: example/action@abc # pinned
        with:
          token: "${{ secrets.TOKEN }}"
  first:
    runs-on: ubuntu-latest
    steps: []
"""
        actual = normalize_yaml(".github/workflows/demo.yml", source)
        assert (actual) == (expected)
        assert (normalize_yaml(".github/workflows/demo.yml", actual)) == (actual)

    def test_issue_form_retains_question_and_option_order(self) -> None:
        """Place item and attribute descriptions before options and validation."""
        source = b"""body:
  - attributes:
      options: [second, first]
      label: Choice
      description: Choose one
    validations:
      required: true
    id: choice
    type: dropdown
  - attributes:
      value: |
        Read this first.
    type: markdown
title: "Report: "
description: Tell us
name: Report
"""
        expected = b"""name: Report
description: Tell us
title: "Report: "
body:
  - type: dropdown
    id: choice
    attributes:
      label: Choice
      description: Choose one
      options: [second, first]
    validations:
      required: true
  - type: markdown
    attributes:
      value: |
        Read this first.
"""
        actual = normalize_yaml(".github/ISSUE_TEMPLATE/report.yml", source)
        assert (actual) == (expected)
        assert (normalize_yaml(".github/ISSUE_TEMPLATE/report.yml", actual)) == (actual)

    def test_mkdocs_retains_ordered_navigation_plugin_and_palette_mappings(self) -> None:
        """Arrange the configuration groups without alphabetizing consumers."""
        source = b"""nav:
  Zed: z.md
  Alpha: a.md
plugins:
  zed: {}
  alpha: {}
markdown_extensions:
  zed: {}
  alpha: {}
extra:
  social: [second, first]
theme:
  palette:
    zed: {}
    alpha: {}
validation:
  links: {}
strict: true
site_dir: out
docs_dir: docs
repo_url: https://example.test/repo
site_name: Demo
not_in_nav: |
  /hidden.md
"""
        expected = b"""site_name: Demo
repo_url: https://example.test/repo
docs_dir: docs
site_dir: out
strict: true
validation:
  links: {}
theme:
  palette:
    zed: {}
    alpha: {}
extra:
  social: [second, first]
markdown_extensions:
  zed: {}
  alpha: {}
plugins:
  zed: {}
  alpha: {}
nav:
  Zed: z.md
  Alpha: a.md
not_in_nav: |
  /hidden.md
"""
        actual = normalize_yaml("mkdocs.yml", source)
        assert (actual) == (expected)
        assert (normalize_yaml("mkdocs.yml", actual)) == (actual)

    def test_missing_domain_fields_are_not_filled_in(self) -> None:
        """Ordering leaves incomplete documents incomplete for their validator."""
        source = b"body:\n  - type: input\nname: Incomplete\n"
        assert (normalize_yaml(".github/ISSUE_TEMPLATE/report.yml", source)) == (
            b"name: Incomplete\nbody:\n  - type: input\n"
        )

    def test_template_keeps_block_scalar_chomping_and_placeholders(self) -> None:
        """Retain trailing literal newlines and folded scalar spelling."""
        source = b"description: |+\n  @DESCRIPTION@\n\n\ntitle: >-\n  @TITLE@\n"
        expected = b"title: >-\n  @TITLE@\ndescription: |+\n  @DESCRIPTION@\n\n\n"
        actual = normalize_yaml("platforms/demo/linux/bindings/demo.yaml.in", source)
        assert (actual) == (expected)
        assert (normalize_yaml("platforms/demo/linux/bindings/demo.yaml.in", actual)) == (actual)

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
            normalize_yaml("mkdocs.yml", b"{nav: [], site_name: Demo}\n")
