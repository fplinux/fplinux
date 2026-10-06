# SPDX-License-Identifier: GPL-2.0-only
"""Whole-section and whole-row ordering without changing phone support claims."""

from __future__ import annotations

from fplinux_cli.quality.formatting.canonical_markdown import normalize_markdown


class PhoneMarkdownTests:
    """Keep support notes, links and template values attached to their rows."""

    def test_phone_sections_and_rows_keep_their_claims(self) -> None:
        """Shuffled rows move together with the support state, link and note."""
        source = (
            b"# Phone\n\n## Features\n\n"
            b"| Feature | Hardware | FPLinux | This phone |\n"
            b"| --- | --- | --- | --- |\n"
            b"| Camera | Present | Partial | Rear sensor. |\n"
            b"| [USB networking](usb.md) | Present | Supported | USB only. |\n"
            b"\n## Identity\n\nName stays.\n\n## Status\n\nLimits stay.\n"
        )
        expected = (
            b"# Phone\n\n## Identity\n\nName stays.\n\n## Status\n\nLimits stay.\n\n"
            b"## Features\n\n"
            b"| Feature                  | Hardware | FPLinux   | This phone   |\n"
            b"| ------------------------ | -------- | --------- | ------------ |\n"
            b"| [USB networking](usb.md) | Present  | Supported | USB only.    |\n"
            b"| Camera                   | Present  | Partial   | Rear sensor. |\n"
        )
        result = normalize_markdown("targets/phone/README.md", source)
        assert (result) == (expected)
        assert (normalize_markdown("targets/phone/README.md", result)) == (result)

    def test_placeholder_template_keeps_optional_sections_absent(self) -> None:
        """Ordering preserves placeholders and does not populate missing sections."""
        source = b"# @DEVICE@\n\n## Status\n\nUnknown.\n\n## Identity\n\n@TARGET@\n"
        result = normalize_markdown("platforms/demo/target-template/README.md.in", source)
        assert (result) == (b"# @DEVICE@\n\n## Identity\n\n@TARGET@\n\n## Status\n\nUnknown.\n")
        assert (b"## Hardware interfaces") not in (result)

    def test_other_markdown_keeps_its_own_structure(self) -> None:
        """Ordinary documentation retains its own section order."""
        source = b"## Status\n\nIntro.\n\n## Identity\n\nDetails.\n"
        assert (normalize_markdown("docs/guide.md", source)) == (source)

    def test_fenced_headings_and_tables_remain_literal_contents(self) -> None:
        """Code examples are preserved rather than mistaken for document structure."""
        source = (
            b"# Phone\n\n## Identity\n\n```md\n## Status\n"
            b"| b | a |\n| - | - |\n| 2 | 1 |\n```\n\n## Status\n\nUnknown.\n"
        )
        assert (normalize_markdown("targets/phone/README.md", source)) == (source)

    def test_fence_language_text_inside_a_code_block_cannot_close_it(self) -> None:
        """An embedded opening marker retains following literal example headings."""
        source = (
            b"# Phone\n\n## Status\n\n```markdown\n```python\n## Identity\n"
            b"This heading is literal example text.\n```\n\n## Features\n\n"
            b"No support claim changed.\n"
        )
        assert (normalize_markdown("targets/phone/README.md", source)) == (source)
