# Shell

Use POSIX shell unless a current script requires a Bash feature. The exact
shebang selects the formatter and checker dialect:

- `#!/bin/sh`, `#!/usr/bin/env sh` and `#!/sbin/openrc-run` select POSIX shell;
- `#!/usr/bin/env bash` selects Bash.

The sourced configurations `alpine/abuild.conf` and
`alpine/aports/fplinux-micropythonos-storage/micropythonos.conf` also declare a
POSIX dialect without a shebang. Other `.conf` files are not assumed to be shell.

Recognized scripts and configurations are formatted with `shfmt` and checked by
ShellCheck with all checks enabled at warning severity. Only the two sourced
configurations omit the unused-variable check, since their assignments are read
by the sourcing program. Alpine `APKBUILD` files are a separate DSL: follow Alpine
packaging conventions and the repository's `apkbuild-lint` gate rather than
treating them as ordinary standalone scripts.

## Structure and expansion

Use functions to name meaningful operations, cleanup boundaries and repeated
behavior. Keep the main path readable in execution order. Prefer simple `case`,
`if` and loop constructs over nested command substitutions or dynamically built
shell programs.

Quote expansions unless intentional field splitting or globbing is part of the
contract. When splitting is required, constrain the accepted input and place a
narrow ShellCheck directive at the operation with a reason. Use `--` where the
utility supports it and an input could otherwise be parsed as an option.

Choose variable names that include the owning component or operation when the
scope is broad. Keep short local loop and status names where their function
provides the context. Environment variables that cross a process boundary use
the established uppercase project or component prefix.

Do not assume Bash arrays, `[[ ... ]]`, `pipefail` or process substitution in a
POSIX or OpenRC script. Conversely, do not hide a real Bash dependency behind a
POSIX shebang. `set -e` and traps are control-flow tools, not boilerplate: use
them only where their behavior across functions, conditionals and sourced files
matches the script's contract.

## Processes and cleanup

Make child-process ownership explicit. Record the child PID, forward only the
signals the wrapper is responsible for, wait for the child, and restore or
release mounted filesystems and other acquired state on every applicable exit.
Keep signal handlers and traps small; perform ordinary cleanup in a named
function where possible.

Check the status of operations that establish required state. Preserve the
original failure when cleanup commands can also fail. A sourced configuration
is code: constrain its location and ownership according to the consumer's trust
boundary rather than treating quoting as validation.

## Shell fixtures

Keep standalone fixture scripts focused on one external behavior and give them
an explicit shebang. Prefer a named fixture file over a long shell program
embedded inside a Python string. Expose only the inputs and outputs the test
needs, and follow the shared [test contract](../CODE_STYLE.md#tests) for
independent expectations and evidence levels.
