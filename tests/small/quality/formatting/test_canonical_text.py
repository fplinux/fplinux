# SPDX-License-Identifier: GPL-2.0-only
"""Byte-level contracts for bounded source text normalization."""

from __future__ import annotations

from pathlib import Path

import pytest
from fplinux_cli.quality.formatting.canonical_text import normalize_text

FIXTURES = Path(__file__).parents[3] / "fixtures/formatting/text"


class DtsPropertyOrderTests:
    """Exercise complete DTS statements with literal expected property bytes."""

    @pytest.mark.parametrize(
        ("case_name", "logical_path"),
        [
            pytest.param("property-groups", "board.dts", id="property-groups"),
            pytest.param("statement-values", "values.dtsi", id="statement-values"),
            pytest.param("syntax-barriers", "order.dts", id="syntax-barriers"),
            pytest.param("property-comments", "comments.dts", id="property-comments"),
            pytest.param("section-comments", "comments.dts", id="section-comments"),
            pytest.param("deletion-barriers", "delete.dts", id="deletion-barriers"),
            pytest.param("template-references", "board.dts.in", id="template-references"),
            pytest.param("macro-continuation", "macro.dts", id="macro-continuation"),
        ],
    )
    def test_canonical_bytes_preserve_values_and_order(
        self, case_name: str, logical_path: str
    ) -> None:
        """Keep exact scalar, comment and sequence bytes while ordering known fields."""
        case = FIXTURES / case_name
        source = (case / "input.dts.fixture").read_bytes()
        expected = (case / "expected.dts.fixture").read_bytes()

        actual = normalize_text(logical_path, source)

        assert actual == expected
        assert normalize_text(logical_path, actual) == actual

    @pytest.mark.parametrize(
        "source",
        [
            b"&node { zeta = <2>; alpha = <1>; };\n",
            b"&node {\n\tzeta = <2>;\n\talpha = <1>;\n\tzeta = <3>;\n};\n",
            b'&node {\n\tzeta = "unterminated;\n\talpha = <1>;\n};\n',
            b"&node {\n\tzeta = <2>\n\tchild { alpha = <1>; };\n};\n",
            b"&node {\n\tzeta = <2>;\n\talpha = <1>;\n",
        ],
        ids=[
            "compact-body",
            "duplicate-property",
            "unterminated-string",
            "incomplete-property",
            "incomplete-node",
        ],
    )
    def test_unsafe_or_incomplete_syntax_is_unchanged(self, source: bytes) -> None:
        """Skip unknown bytes, duplicate properties and compact inline bodies."""
        assert (normalize_text("unsafe.dts", source)) == (source)


class ConservativeWhitespaceTests:
    """Keep literal values and executable ordering while completing text files."""

    def test_shell_metadata_and_multiline_values_only_gain_final_newline(self) -> None:
        """Preserve literal blanks, expansion order and command continuations."""
        source = (
            b"pkgname=example  \npkgver=1\n"
            b'source="one  \n\tsecond \n"\n'
            b'builddir="$srcdir/$pkgname-$pkgver"\n'
            b"build() {\n\tprintf '%s\\n' 'literal  ' \\\n"
            b'\t\t"$pkgver"  \n}'
        )
        expected = source + b"\n"
        actual = normalize_text("alpine/aports/example/APKBUILD", source)
        assert (actual) == (expected)
        assert (normalize_text("alpine/aports/example/APKBUILD", actual)) == (expected)

    @pytest.mark.parametrize(
        "source",
        [
            b"build() {\ncat <<'END'\n  literal  \nEND\n}",
            b'pkgdesc="incomplete',
            b"echo continued \\",
        ],
        ids=["heredoc", "unterminated-string", "continuation"],
    )
    def test_heredocs_and_incomplete_shell_literals_are_unchanged(self, source: bytes) -> None:
        """Avoid changing data when the shell's literal boundary is unproven."""
        assert (normalize_text("APKBUILD", source)) == (source)

    @pytest.mark.parametrize(
        ("relative", "source"),
        [
            ("board.Makefile.in", b"NAME := value  \ninclude first.mk\nobj-y += b.o a.o"),
            ("Config.in", b'config DEMO\n\tbool "Demo"'),
            ("Kconfig", b'config DEMO\n\tbool "Demo  "\n\thelp\n\t  Help text  '),
            ("objects.mk", b"obj-y += second.o first.o  "),
            ("payload.s.in", b'.ascii "literal  "\n.word @VALUE@'),
            ("payload.S", b'.ascii "literal  "\n.word 1'),
            ("README.txt.in", b"Target: @TARGET@  "),
        ],
        ids=[
            "make-template",
            "config",
            "kconfig",
            "make-fragment",
            "assembly-template",
            "assembly",
            "text-template",
        ],
    )
    def test_make_kconfig_assembly_and_plain_template_preserve_internal_whitespace(
        self, relative: str, source: bytes
    ) -> None:
        """A trailing newline does not reorder instructions or trim literal data."""
        assert (normalize_text(relative, source)) == (source + b"\n")

    @pytest.mark.parametrize(
        ("relative", "source", "expected"),
        [
            (
                ".editorconfig",
                b"root = true  \n[*.py] \nindent_size = 4 \n[*] \nindent_size = 2\t",
                b"root = true\n[*.py]\nindent_size = 4\n[*]\nindent_size = 2\n",
            ),
            (
                "settings.ini",
                b'[first]  \nvalue = "literal  "  \n\tcontinuation  \n[second]\nvalue=2 ',
                b'[first]\nvalue = "literal  "\n\tcontinuation  \n[second]\nvalue=2\n',
            ),
            (
                "config.fragment.in",
                (
                    b'CONFIG_DEMO="literal  "  \n# CONFIG_OLD is not set  \n'
                    b"CONFIG_HEX=0x0010\t\n@UNKNOWN@  "
                ),
                (
                    b'CONFIG_DEMO="literal  "\n# CONFIG_OLD is not set\n'
                    b"CONFIG_HEX=0x0010\n@UNKNOWN@  \n"
                ),
            ),
        ],
        ids=["editorconfig", "ini", "config-template"],
    )
    def test_ini_and_config_fragment_trim_only_declarative_line_endings(
        self, relative: str, source: bytes, expected: bytes
    ) -> None:
        """Keep section precedence and quoted values while removing outer blanks."""
        actual = normalize_text(relative, source)
        assert (actual) == (expected)
        assert (normalize_text(relative, actual)) == (expected)

    @pytest.mark.parametrize(
        ("relative", "source"),
        [
            ("notes.txt", b"literal  "),
            ("unknown.conf", b"value=1  "),
            ("board.dts", b"\xff\xfe"),
            ("Kconfig", b"a\x00b"),
            ("settings.ini", b"value=1\r\n"),
        ],
        ids=["unknown-text", "unknown-config", "invalid-utf8", "nul", "crlf"],
    )
    def test_unknown_binary_and_non_lf_inputs_are_unchanged(
        self, relative: str, source: bytes
    ) -> None:
        """Leave bytes outside the recognized current text contracts alone."""
        assert (normalize_text(relative, source)) == (source)
