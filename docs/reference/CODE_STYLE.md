# Code style

This is the contributor contract for project-owned source and tests. It favors
code whose intent, data flow and safety properties can be understood from the
current interfaces. Generated, vendored and downloaded sources keep the style
and constraints of their owner; small FPLinux adapters follow the boundary of
the project they are compiled into.

The canonical commands for formatting, checking and building are in
[Building FPLinux](../guides/BUILDING.md). Run the checks and affected builds
that exercise the real language and runtime boundary of a change.

## General principles

### Make the main behavior obvious

Use names that state the operation and domain. Prefer explicit arguments,
keyword arguments for otherwise ambiguous policy choices, and named
intermediate values over compact expressions that hide ordering or errors.
Separate preparation, execution and result collection when those steps have
different effects or failure modes. A short helper with one caller is useful
when it gives a meaningful name to such a boundary.

Keep public command construction readable. Group repeated `argparse` options
only when the commands have the same choices, defaults and mutual-exclusion
rules; sharing parser setup must not change accepted arguments, help text or
dispatch behavior. The same rule applies to shell command builders and TOML
fixtures: make decisive inputs visible instead of burying them in positional
lists or nested executable strings.

### Give shared behavior one owner

Share behavior when current consumers have the same inputs, results, error
semantics, side effects and lifetime rules. Let callers supply real policy
differences explicitly. Do not combine similar-looking operations when they
serve different runtimes, durability guarantees, external ABIs or standalone
delivery boundaries.

Use existing project code before adding a local copy. In particular, common
file hashing, linker-map parsing and atomic file publication should use their
existing shared owner when the semantics match. Atomic publication callers must
still choose the required file mode and durability policy; a caller that
requires synchronized file contents before replacement is not equivalent to one
that requires only atomic replacement. Keep a standalone program self-contained
when importing project code would break how it is built or distributed.

Remove wrappers, duplicate validation and optional behavior that have no current
consumer. Do not add a registry, mode switch or extension point for a possible
future target. Preserve separate implementations when they express different
policies or provide independent evidence.

### Prefer an existing sufficient facility

Look for a solution in this order:

1. Existing code that already owns the behavior.
2. The language standard library.
3. The native kernel, POSIX or platform API for the relevant runtime.
4. An installed project dependency.
5. A small direct implementation.

Verify differences in accepted inputs, errors, side effects, resource lifetime
and packaging before replacing custom code. For example, use the standard
streaming file-digest API where the supported Python runtime provides it, but do
not force that dependency into a separately delivered tool.

### Preserve contracts and safety

Keep public CLI arguments, output, exit behavior, file formats and external ABI
names stable unless the change explicitly updates that contract. Validation,
range checks, atomic replacement, filesystem synchronization, resource cleanup
and concurrency control are not incidental style: retain them where they protect
data, hardware state or a supported workflow.

Make ownership and lifetime visible. Pair acquisition with cleanup, restore
borrowed system state, and keep policy choices at the caller that owns them.
Do not generalize a kernel, bootstrap, phone or host assumption across runtime
boundaries without evidence that the other consumer supports it.

Comments and docstrings explain a non-obvious invariant, external constraint,
safety property or interface. Do not narrate obvious statements or retain a
history of refactors. Maintained documentation describes current supported
behavior and links to the canonical owner of a workflow instead of copying it.

## Language guides

- [C](style/C.md) covers the kernel, pre-Linux bootstrap, phone userspace,
  host tools, embedded adapters and C harnesses.
- [Python](style/PYTHON.md) covers the host CLI, build tooling and Python tests.
- [Shell](style/SHELL.md) covers POSIX, Bash and OpenRC scripts, plus the
  separate Alpine `APKBUILD` boundary.

Formatting is necessary but not sufficient. A formatted host compile does not
establish an ARM, musl, kernel, bootstrap, package or physical-device result.

## Tests

### Test a stable behavior

Each test protects a consumer-visible behavior, invariant, error, safety
property or public artifact. Its name should state the operation, material
scenario and expected result. Before adding a case, identify the concrete
regression that would stop being detected if the test were removed.

Exercise production code through a normal import, object link, process, public
entry point or built artifact appropriate to the claim. Do not extract private
functions with source or AST surgery, include a production C file directly in a
harness, or parse source text and patches as a substitute for behavior.

Regression tests require a reachable failing scenario or direct causal proof,
the violated contract and the observable harm before the fix. Test the restored
behavior, not the spelling of the patch.

### Keep the oracle independent

Expected results come from a specification, literal example, independently
trusted implementation or separately measured result. Do not calculate expected
values with the production constant, registry, helper or algorithm that produces
the actual value.

Share fixture construction, process control and comparison mechanics when that
makes scenarios clearer, but keep decisive expected data independent. Binary
FDT fixtures, for example, are constructed by test-owned format code rather than
the production parser. Small C transformation cases use reviewable literal maps
instead of copying the production transformation algorithm.

Reject change detectors that freeze a current source list, private call order,
configuration spelling or implementation registry. An exact inventory belongs
in a test only when the inventory itself is a public format, protocol, release
manifest or security allowlist.

### Make fixtures and execution readable

Keep small inputs and expectations beside the case. Move substantial process,
shell or TOML setup into a focused named fixture when inline construction hides
the scenario. A fixture states the external boundary it replaces and exposes
the values that matter to the assertion; it does not become a generic framework
for hypothetical tests.

For process-heavy cases, use named preparation, execution and collection steps
when they make resource ownership and failures easier to follow. Control paths,
environment, locale, time, randomness and subprocess lifetime. Use unique
temporary resources, support independent and parallel runs, and clean up only
resources owned by the case.

Prefer real temporary files, processes, locks and production objects when they
are deterministic and practical. Mock external, expensive, unavailable or
nondeterministic boundaries. Do not mock the central subject and then assert
that the mock was called; assert observable values, files, state transitions,
messages, artifacts or effects instead.

### State the evidence level

Pure units, host components, mocked integrations, package inspections, public
workflows and physical hardware runs establish different facts. Test names,
docstrings and reports must describe the boundary actually exercised. A host C
harness is not a framebuffer or phone test, and a successful build is not a
hardware test.

Keep tests hermetic and deterministic unless their declared purpose is to
exercise an external integration. Tests must not silently depend on ambient
credentials, a developer home directory, live network access, incidental cache,
execution order or previous test results.
