# Python

Project host tooling and tests run on Python 3.14. Ruff checks Python files in
the MicroPythonOS aport using its `py311` syntax target, while the phone runtime
is the pinned MicroPython implementation and supports its own Python subset.
Do not carry CPython standard-library assumptions into that embedded boundary;
verify them against its actual package build and runtime.

The repository configuration is authoritative: Ruff formats and lints Python
with a 99-column limit, and mypy runs in strict mode. Keep suppressions narrow,
next to the exceptional boundary, and explain why the normal rule is unsuitable.

## Interfaces and flow

Use type annotations for maintained interfaces and dataclasses for records with
named fields and useful value semantics. Prefer `pathlib.Path` for filesystem
paths and the most specific collection or callable type that describes the
boundary. Put imports needed only by annotations behind `TYPE_CHECKING` when
that avoids a runtime dependency.

Keep orchestration readable from the entry point. Give preparation, execution
and result collection separate named steps when they have distinct effects,
resources or failure modes. Prefer explicit loops and intermediate values when
a comprehension or nested expression would hide ordering, mutation or an error
path.

Use keyword-only parameters for policy choices and other arguments whose meaning
is not obvious at the call site. A flat explicit interface is preferable to a
generic options mapping or a long sequence of positional booleans. A lint limit
may be suppressed when the parameters represent distinct boundary inputs and a
wrapper would only hide them.

For `argparse`, share an option group only across commands with the same choices,
defaults, mutual exclusion and validation. Keep user-facing help specific to the
command and preserve the existing parsed namespace and dispatch semantics.

## Reuse and file operations

Use the standard library before writing a local algorithm. Stream files through
`hashlib.file_digest()` on the supported host runtime. Use the existing common
SHA-256, linker-map and atomic-publication helpers when their contract matches
the caller instead of retaining identical local implementations.

Atomic file publication must create the temporary file beside its destination,
set the intended final mode, replace only after a complete write, and clean up a
failed temporary. The caller chooses whether the content requires `flush()` and
`fsync()` before replacement. Do not weaken an established durability policy
merely to share the mechanism.

Keep parsing separate from policy. A shared parser may normalize one format;
the caller still owns which symbols, paths, digests or ranges are required and
the error presented at its public boundary.

Use the repository's established failure and output interfaces rather than
mixing ad hoc `print()`, exceptions and exit conversion inside domain helpers.
Cleanup must preserve the error that belongs to the failed operation.

## Tests

Follow the shared [test contract](../CODE_STYLE.md#tests). Import production
modules normally. Use named records and fixtures when they make process state,
shell commands or TOML inputs easier to inspect, but keep short decisive values
literal in the case. Do not import production constants or algorithms to derive
expected data.

Python tests are divided by the boundary they actually exercise. Keep a unit or
host-process result distinct from artifact, public-workflow and hardware
evidence, regardless of which test runner invokes it.
