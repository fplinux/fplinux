# SPDX-License-Identifier: GPL-2.0-only
# ruff: noqa: S101 -- assertions verify the isolated bundled interpreter.
"""Check the standard-library extensions required by standalone archives."""

import cmath
import ctypes
import decimal
import hashlib
import json  # noqa: F401 -- importing the module is part of the runtime check.
import lzma
import math
import sys
import tomllib
from compression import zstd

assert sys.version_info[:3] == tuple(int(part) for part in sys.argv[1].split("."))
assert hashlib.sha256(b"test").hexdigest() == (
    "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
)
assert lzma.decompress(lzma.compress(b"test")) == b"test"
assert zstd.decompress(zstd.compress(b"test")) == b"test"
assert decimal.Decimal("0.1") + decimal.Decimal("0.2") == decimal.Decimal("0.3")
assert tomllib.loads("value=42")["value"] == 42
assert math.sqrt(81) == 9
assert cmath.sqrt(-1) == 1j
assert ctypes.sizeof(ctypes.c_uint64) == 8
