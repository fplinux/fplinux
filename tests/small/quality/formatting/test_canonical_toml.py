# SPDX-License-Identifier: GPL-2.0-only
"""In-memory TOML ordering preserves values, comments and collection ownership."""

from __future__ import annotations

import tomllib

import pytest
from fplinux_cli.quality.formatting.canonical_toml import normalize_toml


class CanonicalTomlTests:
    """Compare shuffled documents with independent canonical examples."""

    def assert_normalized(
        self, relative: str, source: str, expected: str, *, same_values: bool = True
    ) -> bytes:
        """Check literal output and repeatability, with explicit array exceptions."""
        actual = normalize_toml(relative, source.encode())
        assert (actual) == (expected.encode())
        assert (normalize_toml(relative, actual)) == (actual)
        if same_values:
            assert (tomllib.loads(actual.decode())) == (tomllib.loads(source))
        return actual

    def test_target_fields_keep_license_comments_hex_and_placeholders(self) -> None:
        """Reordering retains field annotations and unknown optional inputs."""
        self.assert_normalized(
            "platforms/demo/target-template/target.toml.in",
            """# SPDX-License-Identifier: GPL-2.0-only
platform = "demo"

[adapter]
exec_distance = 0x0040 # distance
# Display level
backlight_level = 0x0f
spi_mode = 1
unknown_z = "@TARGET@"
unknown_a = "%(target)s"

[linux]
memory = { size = 0x0100, base = 0x8000 }
config_fragment = "@CONFIG@"
""",
            """# SPDX-License-Identifier: GPL-2.0-only
platform = "demo"

[linux]
config_fragment = "@CONFIG@"
memory = { base = 0x8000, size = 0x0100 }

[adapter]
spi_mode = 1
# Display level
backlight_level = 0x0f
exec_distance = 0x0040 # distance
unknown_z = "@TARGET@"
unknown_a = "%(target)s"
""",
        )

    def test_section_comments_move_with_their_section(self) -> None:
        """A section annotation remains beside its header after section sorting."""
        self.assert_normalized(
            "targets/demo/target.toml",
            """# SPDX-License-Identifier: GPL-2.0-only
platform = "demo"

# Device inputs
[board_maps]
firmware = []

# Loader controls
[adapter]
session_name = "demo"
exec_distance = 0
""",
            """# SPDX-License-Identifier: GPL-2.0-only
platform = "demo"

# Loader controls
[adapter]
exec_distance = 0
session_name = "demo"

# Device inputs
[board_maps]
firmware = []
""",
        )

    def test_inline_firmware_tables_keep_ordered_array_and_scalar_spellings(self) -> None:
        """Inline fields change order while firmware records and their values stay fixed."""
        self.assert_normalized(
            "targets/demo/target.toml",
            """[bluetooth]
firmware = [
  { sha256 = 'aBcD', size = 0x0F, destination = 'z', source = "first" }, # First record
  { destination = "a", source = "second", size = 1_000 },
]
""",
            """[bluetooth]
firmware = [
  { source = "first", destination = 'z', size = 0x0F, sha256 = 'aBcD' }, # First record
  { source = "second", destination = "a", size = 1_000 },
]
""",
        )

    def test_target_package_sets_sort_without_sorting_other_arrays(self) -> None:
        """Only package sets change element order; duplicate values remain duplicated."""
        source = """[bundle]
packages = ["z", "a", "a"]

[identity]
hardware_codes = ["Z-2", "A-1"]

[rootfs]
packages = ["z", "a"]

[display_brightness]
levels = [20, 4, 0]
"""
        actual = normalize_toml("targets/demo/target.toml", source.encode())
        assert (tomllib.loads(actual.decode())) == (
            {
                "identity": {"hardware_codes": ["Z-2", "A-1"]},
                "display_brightness": {"levels": [20, 4, 0]},
                "rootfs": {"packages": ["a", "z"]},
                "bundle": {"packages": ["a", "a", "z"]},
            }
        )
        assert (normalize_toml("targets/demo/target.toml", actual)) == (actual)

    def test_package_comments_remain_attached_to_sorted_elements(self) -> None:
        """A package's annotation travels with that package rather than its old slot."""
        self.assert_normalized(
            "platforms/demo/platform.toml",
            """[rootfs]
packages = [
  "z", # Z package
  "a", # A package
]
""",
            """[rootfs]
packages = [
  "a", # A package
  "z", # Z package
]
""",
            same_values=False,
        )

    def test_leading_package_annotations_follow_their_sorted_element(self) -> None:
        """Comments on their own lines remain associated with the following package."""
        self.assert_normalized(
            "targets/demo/target.toml",
            """[rootfs]
packages = [
  # Z package
  "z",
  # A package
  "a"
]
""",
            """[rootfs]
packages = [
  # A package
  "a",
  # Z package
  "z",
]
""",
            same_values=False,
        )

    def test_platform_copy_fragments_group_at_linux_and_preserve_sequence(self) -> None:
        """Separated parent fragments combine without sorting copies or appends."""
        self.assert_normalized(
            "platforms/demo/platform.toml",
            """[linux]
patches = ["z.patch", "a.patch"]
arch = "arm"

[[linux.appends]]
destination = "out-a"
source = "in-a"

[[linux.copies]]
destination = "out-z"
source = "in-z"

[runtime]
fdl1_load_address = 0x00006200

[[linux.copies]]
destination = "out-b"
source = "in-b"
""",
            """[linux]
arch = "arm"
patches = ["z.patch", "a.patch"]

[[linux.copies]]
source = "in-z"
destination = "out-z"

[[linux.copies]]
source = "in-b"
destination = "out-b"

[[linux.appends]]
source = "in-a"
destination = "out-a"

[runtime]
fdl1_load_address = 0x00006200
""",
        )

    def test_host_recipe_children_remain_owned_by_their_parent_record(self) -> None:
        """Each recipe keeps its own members and copies after ordinary fields."""
        self.assert_normalized(
            "platforms/demo/platform.toml",
            """[[host.tools]]
self_test = false
patches = []
name = "first"
type = "make-archive"

[[host.tools.copies]]
destination = "first-out"
source = "first-in"

[[host.tools.members]]
digest_key = "first-sha"
path = "first.c"

[[host.tools]]
self_test = true
copies = []
name = "second"
type = "make-archive"

[[host.tools.members]]
digest_key = "second-sha"
path = "second.c"
""",
            """[[host.tools]]
type = "make-archive"
name = "first"
patches = []
self_test = false

[[host.tools.members]]
path = "first.c"
digest_key = "first-sha"

[[host.tools.copies]]
source = "first-in"
destination = "first-out"

[[host.tools]]
type = "make-archive"
name = "second"
copies = []
self_test = true

[[host.tools.members]]
path = "second.c"
digest_key = "second-sha"
""",
        )

    def test_loader_output_order_and_comments_stay_with_each_source(self) -> None:
        """Moving fields never reassigns outputs to a neighboring source."""
        self.assert_normalized(
            "targets/demo/loader/assets.lock.toml",
            """[[source]]
kind = "file"
id = "z"

[[source.output]]
sha256 = "z-sha"
path = "z.bin"
role = "z-role"

# Second source
[[source]]
kind = "7z"
id = "a"

[[source.output]]
sha256 = "a-sha"
member = "a/member.bin"
path = "a.bin"
role = "a-role"
""",
            """[[source]]
id = "z"
kind = "file"

[[source.output]]
role = "z-role"
path = "z.bin"
sha256 = "z-sha"

# Second source
[[source]]
id = "a"
kind = "7z"

[[source.output]]
role = "a-role"
path = "a.bin"
member = "a/member.bin"
sha256 = "a-sha"
""",
        )

    def test_independent_dictionaries_use_case_sensitive_lexical_order(self) -> None:
        """Addressable dictionaries sort full keys while their array values retain order."""
        self.assert_normalized(
            "_typos.toml",
            """[default.extend-words]
z10 = "ten"
z2 = "two"
Zoo = "upper"

[files]
extend-exclude = ["z*", "a*"]
""",
            """[files]
extend-exclude = ["z*", "a*"]

[default.extend-words]
Zoo = "upper"
z10 = "ten"
z2 = "two"
""",
        )

    def test_python_tool_rules_keep_glob_precedence(self) -> None:
        """Fixed tool fields sort without changing the order of external glob rules."""
        self.assert_normalized(
            "pyproject.toml",
            """[tool.mypy]
cache_dir = "/tmp/mypy"
python_version = "3.14"
mypy_path = "scripts"
strict = true

[tool.ruff.lint.per-file-ignores]
"z/**" = ["Z", "A"]
"a/**" = ["B"]

[tool.ruff]
line-length = 99
target-version = "py314"
""",
            """[tool.ruff]
target-version = "py314"
line-length = 99

[tool.ruff.lint.per-file-ignores]
"z/**" = ["Z", "A"]
"a/**" = ["B"]

[tool.mypy]
python_version = "3.14"
strict = true
mypy_path = "scripts"
cache_dir = "/tmp/mypy"
""",
        )

    @pytest.mark.parametrize(
        ("relative", "source", "expected"),
        [
            (
                "container.lock.toml",
                """[oci]
platform = "linux/amd64"
repository = "local/image"

[kern]
binary_sha256 = "binary-hash"
version = "1"
archive_sha256 = "archive-hash"
archive_url = "https://example.invalid/runtime"
""",
                """[kern]
version = "1"
archive_url = "https://example.invalid/runtime"
archive_sha256 = "archive-hash"
binary_sha256 = "binary-hash"

[oci]
repository = "local/image"
platform = "linux/amd64"
""",
            ),
            (
                "sources.lock.toml",
                """[fpdoom_host.files]
z_sha256 = "z"
a_sha256 = "a"

[linux]
license = "GPL-2.0-only"
sha256 = "hash"
url = "https://example.invalid/linux"
version = "1"
""",
                """[linux]
version = "1"
url = "https://example.invalid/linux"
sha256 = "hash"
license = "GPL-2.0-only"

[fpdoom_host.files]
a_sha256 = "a"
z_sha256 = "z"
""",
            ),
            (
                "platforms/demo/uboot/u-boot.lock.toml",
                'license = "GPL-2.0-only"\ncommit = "c"\ntag = "t"\nversion = "v"\n',
                'version = "v"\ntag = "t"\ncommit = "c"\nlicense = "GPL-2.0-only"\n',
            ),
            (
                "targets/demo/release/manifest.toml",
                """documents = ["z.md", "a.md"]
runtime_files = ["z.bin", "a.bin"]
image = "linux.bin"
bundle_files = ["z.apk", "a.apk"]
""",
                """image = "linux.bin"
bundle_files = ["z.apk", "a.apk"]
runtime_files = ["z.bin", "a.bin"]
documents = ["z.md", "a.md"]
""",
            ),
        ],
        ids=["container-lock", "source-lock", "boot-lock", "release-manifest"],
    )
    def test_named_lock_fields_and_release_lists_follow_their_domains(
        self, relative: str, source: str, expected: str
    ) -> None:
        """Pinned inputs use semantic groups; only addressable digest keys sort lexically."""
        self.assert_normalized(relative, source, expected)

    def test_alpine_catalogue_sorts_files_without_sorting_install_lists(self) -> None:
        """Catalogue records and annotations travel together; apk input lists stay ordered."""
        actual = self.assert_normalized(
            "alpine.lock.toml",
            """arch = "armv7"
release = "3.24.0"

[sysroot]
packages = ["z.apk", "a.apk"]

[[package]]
bytes = 2
sha256 = "z-sha"
file = "z.apk"
repository = "main"

# A record
[[package]]
bytes = 1
sha256 = "a-sha"
file = "a.apk"
repository = "community"

[runtime]
packages = ["z.apk", "a.apk"]

[runtime.additions]
z = ["z.apk", "a.apk"]
a = ["a.apk"]
""",
            """release = "3.24.0"
arch = "armv7"

[runtime]
packages = ["z.apk", "a.apk"]

[runtime.additions]
a = ["a.apk"]
z = ["z.apk", "a.apk"]

[sysroot]
packages = ["z.apk", "a.apk"]

# A record
[[package]]
repository = "community"
file = "a.apk"
sha256 = "a-sha"
bytes = 1

[[package]]
repository = "main"
file = "z.apk"
sha256 = "z-sha"
bytes = 2
""",
            same_values=False,
        )
        catalogue = tomllib.loads(actual.decode())["package"]
        assert (catalogue) == (
            [
                {"repository": "community", "file": "a.apk", "sha256": "a-sha", "bytes": 1},
                {"repository": "main", "file": "z.apk", "sha256": "z-sha", "bytes": 2},
            ]
        )

    @pytest.mark.parametrize(
        ("relative", "source", "expected"),
        [
            (
                "tests/fixtures/profile_config/default-with-feature-package.toml",
                """[rootfs]
packages = ["feature", "z", "a"]

[linux.root]
kind = "initramfs"
""",
                """[linux.root]
kind = "initramfs"

[rootfs]
packages = ["feature", "z", "a"]
""",
            ),
            (
                "tests/fixtures/profile_config/target-without-microsd.toml",
                """[adapter]
session_name = "%(target)s"

[nand]
raw_device = "/dev/demo-raw"
""",
                """[nand]
raw_device = "/dev/demo-raw"

[adapter]
session_name = "%(target)s"
""",
            ),
            (
                "alpine.lock.toml",
                """[[package]]
bytes = -1
file = "z.apk"

[[package]]
sha256 = "invalid"
""",
                """[[package]]
file = "z.apk"
bytes = -1

[[package]]
sha256 = "invalid"
""",
            ),
            (
                "targets/demo/target.toml",
                '[rootfs]\npackages = [["z", "a"], "b"]\n',
                '[rootfs]\npackages = [["z", "a"], "b"]\n',
            ),
        ],
        ids=[
            "feature-package",
            "missing-microsd",
            "negative-package-input",
            "nested-package-list",
        ],
    )
    def test_profile_packages_and_negative_optional_inputs_are_preserved(
        self, relative: str, source: str, expected: str
    ) -> None:
        """Formatting cannot fill omissions or remove semantically invalid feature packages."""
        self.assert_normalized(relative, source, expected)

    def test_reuse_annotations_and_paths_remain_ordered(self) -> None:
        """Annotation precedence keeps its original record and path order."""
        self.assert_normalized(
            "REUSE.toml",
            """SPDX-PackageSupplier = "person"
version = 1

[[annotations]]
SPDX-License-Identifier = "MIT"
precedence = "override"
path = ["z/**", "a/**"]

[[annotations]]
SPDX-License-Identifier = "GPL-2.0-only"
path = ["*.py"]
""",
            """version = 1
SPDX-PackageSupplier = "person"

[[annotations]]
path = ["z/**", "a/**"]
precedence = "override"
SPDX-License-Identifier = "MIT"

[[annotations]]
path = ["*.py"]
SPDX-License-Identifier = "GPL-2.0-only"
""",
        )

    def test_unknown_tables_and_values_keep_their_existing_order(self) -> None:
        """Unrecognized data receives no alphabetical or semantic repair."""
        self.assert_normalized(
            "settings.toml",
            '''[custom]
z = +1_000
a = 1979-05-27T07:32:00Z
text = """first

[not.a.table]
last"""
''',
            '''[custom]
z = +1_000
a = 1979-05-27T07:32:00Z
text = """first

[not.a.table]
last"""
''',
        )

    @pytest.mark.parametrize(
        "source",
        [b"[invalid", b"x = 1\nx = 2\n", b"x = '\xff'\n"],
        ids=["invalid-syntax", "duplicate-key", "invalid-utf8"],
    )
    def test_syntax_errors_and_invalid_utf8_are_rejected(self, source: bytes) -> None:
        """Broken syntax is reported instead of rewritten into another document."""
        with pytest.raises(ValueError):  # noqa: PT011 -- stable rejection type; diagnostics vary.
            normalize_toml("targets/demo/target.toml", source)
