# SPDX-License-Identifier: GPL-2.0-only
"""Byte-level contracts for bounded source text normalization."""

from __future__ import annotations

import unittest

from fplinux_cli.quality.formatting.canonical_text import normalize_text


class DtsPropertyOrderTests(unittest.TestCase):
    """Exercise complete DTS statements with literal expected property bytes."""

    def test_property_groups_and_numeric_suffixes_have_canonical_order(self) -> None:
        """Place standard properties before prefixed properties and status."""
        source = b"""/ {
\tdevice@0 {
\t\tstatus = "disabled";
\t\tport12 = <0xc>;
\t\texample,mode10 = <10>;
\t\tranges;
\t\tport2 = <0x02>;
\t\texample,mode2 = <2>;
\t\treg = <0x00 0x100>;
\t\tcompatible = "example,device";
\t\tdevice_type = "test";
\t};
};
"""
        expected = b"""/ {
\tdevice@0 {
\t\tdevice_type = "test";
\t\tcompatible = "example,device";
\t\treg = <0x00 0x100>;
\t\tranges;
\t\tport2 = <0x02>;
\t\tport12 = <0xc>;
\t\texample,mode2 = <2>;
\t\texample,mode10 = <10>;

\t\tstatus = "disabled";
\t};
};
"""
        actual = normalize_text("board.dts", source)
        self.assertEqual(actual, expected)
        self.assertEqual(normalize_text("board.dts", actual), expected)

    def test_arrays_strings_and_inline_comments_move_as_complete_statements(self) -> None:
        """Keep value bytes and a statement's same-line annotation together."""
        source = b"""&device {
\tzeta = "a; { // literal", "second"; // zeta annotation
\treg = <0x0001 0x0010>,
\t      <0x0002 0x0020>;
\talpha = [01 ff 02]; /* alpha annotation */
};
"""
        expected = b"""&device {
\treg = <0x0001 0x0010>,
\t      <0x0002 0x0020>;
\talpha = [01 ff 02]; /* alpha annotation */
\tzeta = "a; { // literal", "second"; // zeta annotation
};
"""
        self.assertEqual(normalize_text("values.dtsi", source), expected)

    def test_include_preprocessing_children_and_overrides_keep_their_positions(self) -> None:
        """Sort local runs without moving declarations across syntax barriers."""
        source = b"""#include "before.dtsi"
&second {
\tzeta = <3>;
\talpha = <1>;
#if ENABLED
\treg = <0x20 4>;
\tcompatible = "example,second";
#endif
\t/include/ "inside.dtsi"
\tchild-b {
\t\tzeta = <3>;
\t\talpha = <1>;
\t};
\tchild-a { value = <2>; };
};
#include "after.dtsi"
&first {
\tvalue = <2>;
};
"""
        expected = b"""#include "before.dtsi"
&second {
\talpha = <1>;
\tzeta = <3>;
#if ENABLED
\tcompatible = "example,second";
\treg = <0x20 4>;
#endif
\t/include/ "inside.dtsi"
\tchild-b {
\t\talpha = <1>;
\t\tzeta = <3>;
\t};
\tchild-a { value = <2>; };
};
#include "after.dtsi"
&first {
\tvalue = <2>;
};
"""
        actual = normalize_text("order.dts", source)
        self.assertEqual(actual, expected)
        self.assertEqual(normalize_text("order.dts", actual), expected)

    def test_contiguous_leading_comments_travel_with_the_annotated_property(self) -> None:
        """Keep comments attached while ordering complete property blocks."""
        source = b"""&device {
\t/* This setting stays beside its explanation. */
\t// Its scalar value remains unchanged.
\tzeta = <0x003>;
\talpha = <1>;
};
"""
        expected = b"""&device {
\talpha = <1>;
\t/* This setting stays beside its explanation. */
\t// Its scalar value remains unchanged.
\tzeta = <0x003>;
};
"""
        actual = normalize_text("comments.dts", source)
        self.assertEqual(actual, expected)
        self.assertEqual(normalize_text("comments.dts", actual), expected)

    def test_separated_section_comments_keep_property_groups_on_their_original_sides(self) -> None:
        """A blank line gives a section comment an unmoved ordering boundary."""
        source = b"""&device {
\tzeta = <3>;
\talpha = <1>;
\t/* A separate group. */

\tzulu = <4>;
\tbeta = <2>;
};
"""
        expected = b"""&device {
\talpha = <1>;
\tzeta = <3>;
\t/* A separate group. */

\tbeta = <2>;
\tzulu = <4>;
};
"""
        self.assertEqual(normalize_text("comments.dts", source), expected)

    def test_deletion_statements_do_not_cross_property_updates(self) -> None:
        """Keep each update on its original side of a deletion directive."""
        source = b"""&device {
\tzulu = <4>;
\talpha = <1>;
\t/delete-property/ value;
\tzeta = <3>;
\tbeta = <2>;
};
"""
        expected = b"""&device {
\talpha = <1>;
\tzulu = <4>;
\t/delete-property/ value;
\tbeta = <2>;
\tzeta = <3>;
};
"""
        self.assertEqual(normalize_text("delete.dts", source), expected)

    def test_path_references_and_template_placeholders_remain_verbatim(self) -> None:
        """Recognize reference braces without treating them as child nodes."""
        source = b"""#include "@TARGET@-identity.dtsi"
&{/soc/device@0} {
\tzeta = <&{/soc/clock@0} 0x01>;
\talpha = "@DEVICE@";
};
"""
        expected = b"""#include "@TARGET@-identity.dtsi"
&{/soc/device@0} {
\talpha = "@DEVICE@";
\tzeta = <&{/soc/clock@0} 0x01>;
};
"""
        self.assertEqual(normalize_text("board.dts.in", source), expected)

    def test_unsafe_or_incomplete_syntax_is_unchanged(self) -> None:
        """Skip unknown bytes, duplicate properties and compact inline bodies."""
        cases = (
            b"&node { zeta = <2>; alpha = <1>; };\n",
            b"&node {\n\tzeta = <2>;\n\talpha = <1>;\n\tzeta = <3>;\n};\n",
            b'&node {\n\tzeta = "unterminated;\n\talpha = <1>;\n};\n',
            b"&node {\n\tzeta = <2>\n\tchild { alpha = <1>; };\n};\n",
            b"&node {\n\tzeta = <2>;\n\talpha = <1>;\n",
        )
        for source in cases:
            with self.subTest(source=source):
                self.assertEqual(normalize_text("unsafe.dts", source), source)

    def test_preprocessor_continuations_preserve_macro_body_bytes(self) -> None:
        """A directive's continued brace and semicolon tokens remain opaque."""
        source = b"#define VALUE \\\n\t{ 1; 2; }\n&node {\n\tzeta = <2>;\n\talpha = <1>;\n};\n"
        self.assertEqual(
            normalize_text("macro.dts", source),
            b"#define VALUE \\\n\t{ 1; 2; }\n&node {\n\talpha = <1>;\n\tzeta = <2>;\n};\n",
        )


class ConservativeWhitespaceTests(unittest.TestCase):
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
        self.assertEqual(actual, expected)
        self.assertEqual(normalize_text("alpine/aports/example/APKBUILD", actual), expected)

    def test_heredocs_and_incomplete_shell_literals_are_unchanged(self) -> None:
        """Avoid changing data when the shell's literal boundary is unproven."""
        cases = (
            b"build() {\ncat <<'END'\n  literal  \nEND\n}",
            b'pkgdesc="incomplete',
            b"echo continued \\",
        )
        for source in cases:
            with self.subTest(source=source):
                self.assertEqual(normalize_text("APKBUILD", source), source)

    def test_make_kconfig_assembly_and_plain_template_preserve_internal_whitespace(self) -> None:
        """A trailing newline does not reorder instructions or trim literal data."""
        cases = (
            ("board.Makefile.in", b"NAME := value  \ninclude first.mk\nobj-y += b.o a.o"),
            ("Config.in", b'config DEMO\n\tbool "Demo"'),
            ("Kconfig", b'config DEMO\n\tbool "Demo  "\n\thelp\n\t  Help text  '),
            ("objects.mk", b"obj-y += second.o first.o  "),
            ("payload.s.in", b'.ascii "literal  "\n.word @VALUE@'),
            ("payload.S", b'.ascii "literal  "\n.word 1'),
            ("README.txt.in", b"Target: @TARGET@  "),
        )
        for relative, source in cases:
            with self.subTest(relative=relative):
                self.assertEqual(normalize_text(relative, source), source + b"\n")

    def test_ini_and_config_fragment_trim_only_declarative_line_endings(self) -> None:
        """Keep section precedence and quoted values while removing outer blanks."""
        cases = (
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
        )
        for relative, source, expected in cases:
            with self.subTest(relative=relative):
                actual = normalize_text(relative, source)
                self.assertEqual(actual, expected)
                self.assertEqual(normalize_text(relative, actual), expected)

    def test_unknown_binary_and_non_lf_inputs_are_unchanged(self) -> None:
        """Leave bytes outside the recognized current text contracts alone."""
        cases = (
            ("notes.txt", b"literal  "),
            ("unknown.conf", b"value=1  "),
            ("board.dts", b"\xff\xfe"),
            ("Kconfig", b"a\x00b"),
            ("settings.ini", b"value=1\r\n"),
        )
        for relative, source in cases:
            with self.subTest(relative=relative):
                self.assertEqual(normalize_text(relative, source), source)


if __name__ == "__main__":
    unittest.main()
