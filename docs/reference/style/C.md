# C

This guide applies to project-owned C in `bootstrap/`, `platforms/`, `targets/`,
`alpine/`, `common/host/` and the C test harnesses.

All project-owned C uses the same Linux kernel formatting rules. Non-kernel code
reads the repository `.clang-format`; kernel checks read the copy in the pinned
Linux tree. Their formatting options are identical. The files differ only in
the SPDX comment, which has no effect on `clang-format`.

The code still runs in different environments. A kernel driver, the pre-Linux
bootstrap, a phone program and a host tool have different APIs, error handling
and lifetime rules. The layer sections below do not define other visual styles;
they explain which language features and interfaces are valid in that runtime.

Downloaded sources keep their own style. A project-owned adapter compiled inside
an upstream tree follows the boundary described below.

## Where the code runs

| Code                                                           | Environment                        | Language and API                                                      |
| -------------------------------------------------------------- | ---------------------------------- | --------------------------------------------------------------------- |
| `platforms/*/kernel/`, `targets/*/kernel/`                     | Linux kernel                       | Kernel GNU C, kernel types and subsystem APIs                         |
| `bootstrap/`, `platforms/*/bootstrap/`, `targets/*/bootstrap/` | Fixed vendor runtime before Linux  | C99 with the facilities supplied by the bootstrap closure             |
| `alpine/aports/`, `alpine/shared/`                             | musl/Linux on the phone            | C11 or the GNU dialect selected by the APKBUILD, POSIX and Linux UAPI |
| `common/host/`                                                 | Linux x86-64 host                  | C11, POSIX, Linux UAPI and libusb                                     |
| Sources marked `fplinux-check: package-embedded`               | TyrQuake or MicroPython build tree | Destination project's dialect and external ABI                        |
| `tests/host_tool/*.c`                                          | Host-only harness                  | The dialect selected by the test that compiles it                     |

Do not move an API or language assumption from one row to another. `errno`, file
descriptors and signals do not belong in the bootstrap. Kernel code does not use
libc types or POSIX calls. A successful host compile does not prove that ARM,
musl, bootstrap or kernel code builds.

## Use the check that understands the layer

Use the canonical [format, check and build procedures](../../guides/BUILDING.md).
The relevant commands establish different things:

- `check c` checks formatting for bootstrap, phone userspace, host C, embedded
  adapter files and C test harnesses. Its Clang analysis covers independently
  compilable phone userspace and host translation units only.
- Bootstrap file formatting is checked by `check c`, but their real compile
  proof is an affected target build.
- Embedded TyrQuake and MicroPython file formatting is checked by `check c`, but
  they are compiled only in their pinned upstream package builds. C fragments
  inside patch files are judged by that destination build, not by standalone
  analysis.
- `check kernel` projects code into the pinned Linux tree and runs its formatter,
  checkpatch, configuration, Devicetree and Sparse checks.
  For C/H changes stored in Linux patches, formatting covers the changed
  regions in their complete source context. Unrelated upstream code is not
  reformatted. Use `format PATH.patch` to regenerate a declared Linux patch.
- `check python` compiles and runs the host C harnesses driven by the unit suite.
  That result remains host-only evidence.

Before submitting a change, run the complete `./fplinux check --no-cache` gate
and build every target affected by bootstrap, package, host-tool or kernel
integration as described in the build guide.

## Rules shared by project C

### Let the formatter handle layout

The repository `.clang-format` defines tabs, braces, continuation indentation
and the 80-column baseline. Do not hand-format around it or add a nested
formatter configuration. Use a short helper or a clearer data structure when a
statement remains difficult to read after formatting.

Put the SPDX identifier first. A required feature-test macro such as
`_GNU_SOURCE` or `_DEFAULT_SOURCE` comes after SPDX and before every header.

Comments explain an invariant, an unusual ordering requirement, an external ABI
or why the obvious approach is unsafe. They do not repeat the next statement or
preserve a history of old implementations.

### Pick the owner before the prefix

| Owner                    | Typical code                                       | Symbol prefix          | Macro prefix           |
| ------------------------ | -------------------------------------------------- | ---------------------- | ---------------------- |
| Shared FPLinux component | boot screen, multitap core, common protocol        | `fplinux_<component>_` | `FPLINUX_<COMPONENT>_` |
| Unisoc UMS9117 platform  | bootstrap flow, timers, MUSB, ADI, LCDC, keypad    | `ums9117_`             | `UMS9117_`             |
| INOI 240 Modern 4G       | board wiring and phone-only policy                 | `inoi240_`             | `INOI240_`             |
| INOI 244 Modern 4G       | board wiring and phone-only policy                 | `inoi244_`             | `INOI244_`             |
| Nokia 3210 4G (TA-1618)  | board wiring and phone-only policy                 | `ta1618_`              | `TA1618_`              |
| Separate chip            | SC2720 registers and fields                        | chip-specific          | `SC2720_`              |
| External ABI             | vendor, TyrQuake, MicroPython or Linux entry point | required spelling      | required spelling      |

The directory does not decide ownership. A portable text composer remains
`fplinux_multitap_*` when the console and MicroPythonOS both use it. A UMS9117
bootstrap helper remains `ums9117_*` when called by a Nokia target. Board values
do not become platform data merely because all current phones happen to share a
number.

Selector-facing runtime device names follow the component that owns the
interface. Do not put a phone model in a power-supply, IIO, thermal, input,
ALSA or misc-device name when one shared driver exposes the same interface and
each target has one instance. Keep a target name when the selector identifies
genuinely different board behavior, such as a panel profile, LCD backlight or
keypad map. Board calibration and descriptive model text remain target-owned
even when the selector name is shared.

An exported or cross-file name carries the owner and component. A `static`
helper can be concise when its file supplies the context: `write_all()`,
`save_display()` and `wait_for_engine()` are clearer than repeating a long
prefix on every private operation.

### Make interfaces and lifetime visible

Use `lower_snake_case` for functions, variables, `struct` tags and `enum` tags.
Use `UPPER_SNAKE_CASE` for macros and enumerators. Prefer named structs and
enums over typedef aliases; typedefs are appropriate for callbacks and required
external APIs.

Name related operations as a pair when they transfer or restore ownership:

```text
init / cleanup
open / close
claim / release
save / restore
start / wait
begin / end
```

Predicates use `is_`, `has_`, `can_` or `_valid`. A function that claims a
resource must make the matching release path clear. A callback that needs
caller state takes an explicit context pointer instead of relying on unrelated
global state.

Keep symbols `static` unless another translation unit has a real reason to call
them. Put that cross-file contract in a header. An `*-internal.h` header is for
a genuine component boundary or a linked test harness, not a way to expose all
private helpers.

### Use types that describe the boundary

- Use `size_t` for object sizes, buffer capacities and byte counts.
- Use fixed-width integer types for wire formats, framebuffer pixels, MMIO and
  other layouts whose width is part of the contract.
- Use the native interface type for operating-system values: `pid_t`, `off_t`,
  `sig_atomic_t` and kernel types are more accurate than an arbitrary integer.
- Check ranges before narrowing, pointer conversion, multiplication or addition.
  Do not add a cast merely to silence a warning.
- Validate a count against the destination capacity before copying or indexing.

Put the unit in an integer name when the type cannot show it:

- `_MS`, `_US`, `_NS` for time;
- `_HZ` for frequency;
- `_BYTES` or `_MMIO_BYTES` for extents;
- `_COUNT` for element counts;
- `_ATTEMPTS` for retry limits;
- `_VENDOR_ID` and `_PRODUCT_ID` for USB IDs.

Hexadecimal digits stay lowercase. When a literal needs a suffix, use uppercase
`U`, `UL` or `ULL`:

```text
0x40608000U
5000U
1UL << bit
```

Do not add suffixes only for decoration. Kernel code uses kernel integer types;
userspace and bootstrap use the standard or interface types established by
their component.

Repository-local header guards are non-reserved and owner-prefixed:

```text
FPLINUX_BOOT_SCREEN_H
FPLINUX_MULTITAP_H
FPLINUX_UMS9117_FB_INTERNAL_H
```

Avoid guards beginning with `__` or an underscore followed by an uppercase
letter. A public header includes the declarations needed for every type it
exposes instead of depending on include order in one consumer.

### Keep command-line parsing consistent

Public C commands use the [shared CLI library](../../../alpine/shared/fplinux-cli.h).
Declare named flags, options with values and positional arguments in one table,
then call `fplinux_cli_parse()`. `-h` and `--help` print generated
syntax and option help to standard output and return zero before the command
opens devices, reads inputs or starts a child. Help takes priority when the rest
of the command line is missing or invalid. Syntax errors and command-owned value
or combination errors go to standard error and return status 2.

A long option value may follow a space or `=`, as in `--output FILE` and
`--output=FILE`. The parser also accepts an unambiguous prefix of a long option;
documentation and scripts use the complete name so they remain clear when an
option table grows.

The command owns its repeated-option policy. Preserve its per-value validation
and final combination checks when retaining the last occurrence of a scalar
option. A scalar which must appear once uses that cardinality in the option
table and rejects duplicates. Keep ordering rules that cross option names,
defaults, numeric ranges and valid combinations in the command that owns them.

The parser resolves syntax, required arguments and help before invoking the
command's option callback. The callback validates and assigns each occurrence
in the original argument order; it does not access resources or run workloads.
The table exposes occurrence counts and borrowed last values for final command
validation. Initialize command defaults before parsing and start work only after
`FPLINUX_CLI_READY`. A command wrapper can opt into an untouched argument tail
after `--`. Parsing does not reorder the caller's argument array or allocate
argument tables.

## Kernel code

Follow the Linux coding style and the API of the subsystem being changed. Use
kernel types, negative errno values, `devm_*` where its lifetime matches, and
`dev_err_probe()` for probe errors that may defer. Logging severity and message
shape belong to the [logging contract](../LOGGING.md).

Execution context is part of a kernel function's contract. Use these suffixes
only with their usual meaning:

- `_locked` means the documented lock is already held;
- `_irq` is an interrupt handler or IRQ-only helper;
- `_work` is a workqueue callback;
- `_probe`, `_remove` and `_shutdown` are driver lifecycle callbacks.

Use `readl()` and `writel()` or the subsystem's accessors for kernel MMIO. A
plain volatile pointer is not a replacement for Linux MMIO ordering. State what
protects shared state and keep long waits outside spinlocked or IRQ-disabled
sections unless the hardware contract requires otherwise.

Per-device state belongs in the device instance. File globals are appropriate
only for immutable tables or a genuinely global registration. Probe, failure
unwind, remove and shutdown must agree about who owns every resource and which
hardware state is restored.

## Bootstrap code

The project bootstrap is C99 built inside a fixed vendor runtime. It is not a
hosted POSIX program. Use only headers and functions supplied by that closure;
do not assume a full libc, file descriptors, threads, signals or `errno`.

Current project bootstrap code keeps state in caller-owned structs, fixed-size
arrays and bounded image regions. Keep it that way unless the bootstrap
architecture explicitly adopts another allocator or lifetime model. A reusable
API copies short-lived input when the caller should not have to retain it.

Callbacks take an explicit `void *context`. Check dimensions, offsets and image
ranges before drawing, copying, personalizing the DTB or touching a controller.
Use subtraction-based bounds checks where adding two untrusted values could
overflow.

Absolute addresses use `_PHYS`. Keep direct volatile MMIO casts inside small,
typed accessors such as `reg_read()` and `reg_write()`. Use `uintptr_t` for the
integer-to-pointer boundary. Ordinary memory and synchronization do not become
safe merely by adding `volatile`.

Machine-facing records such as `*_LINUX_BOOTSTRAP stage=...` are diagnostics,
not the handoff control channel. Do not parse their text to authorize a state
transition. The session-bound binary exchange controls the Linux handoff. A
fatal bootstrap path presents the failure when possible and stops; it must not
continue into Linux with an invalid handoff.

Shared boot flow and required vendor hooks stay in the platform bootstrap. A
target `main.c` supplies board data without copying the common flow.

## Phone userspace

Phone programs use the dialect selected by their APKBUILD, normally C11 and
GNU11 where the component requires it. They may use musl, POSIX and Linux UAPI,
but they still have to validate the actual device and kernel ABI before using
an `ioctl`, `mmap` region or evdev stream.

Use the shared command-line contract above rather than hand-written
flag chains. Keep option effects, numeric ranges and valid combinations in the
program that owns them; a library's integer type is not a replacement for
exact-width or hardware-specific validation.

A parser refactor preserves the command's accepted inputs, option effects and
help behavior unless an interface change is explicitly intended. Parse a
wrapper's own options without altering the child command's arguments.

Initialize resource-owning state so partial cleanup is safe: descriptors start
at `-1`, pointers at `NULL`, and ownership flags at false. Release resources in
reverse acquisition order. Restore grabbed input devices, terminal modes,
framebuffer state and child processes before returning control to the user.

Handle short reads and writes. Retry `EINTR` where the operation remains valid,
and treat `EAGAIN` according to the descriptor mode. Save `errno` before cleanup
when the cleanup calls could replace the error that must be reported.

A signal handler does only async-signal-safe work. The established pattern is to
store the signal number in `volatile sig_atomic_t`; normal control flow then
stops the child, restores state and chooses the exit status.

Use `O_CLOEXEC` unless a descriptor is deliberately inherited. Keep public
command output and exit behavior stable. Message prefixes and severity remain
in the [logging contract](../LOGGING.md).

## Host tools

Host C is not phone C. It must not depend on a target memory map, target name or
an ARM-only behavior. Parse and validate all command-line arguments before
opening, detaching or grabbing a device. An ambiguous USB or evdev match is an
error rather than permission to choose the first device.

Keep error domains straight: a libusb status is not `errno`. Report the API that
failed and translate only at a boundary that defines the translation. On every
exit path, release claimed interfaces, reattach a detached driver when required,
close evdev descriptors and release pressed keys.

Host-tool recipes produce static executables. A recipe that declares
`self_test = true` runs the executable's `--self-test` before publication;
recipes without that declaration are not claimed to self-test.

## Code embedded into another project

An adapter compiled inside TyrQuake, MicroPython or vendor bootstrap keeps the
names and types required by that external ABI. Current examples include
`VID_*`, `IN_*`, `Sys_*`, `MP_*`, `mp_obj_t`, `lcd_appinit()` and
`keytrn_init()`.

Keep required names at the boundary. Use normal project-style `static` helpers
for separable operations inside the adapter, but do not force a naturally
stateful upstream entry point into a one-line wrapper.

Do not reformat downloaded source. A patch follows the destination project's
style. The `fplinux-check: package-embedded` marker tells the standalone host
analyzer that the file needs its upstream compile context; it does not exempt
the file from project formatting. The corresponding APK build is the compile
proof.

## C test harnesses

A C harness includes the declared public or internal header and links the
production object or source as a separate translation unit. Do not include a
production `.c` file into the harness and do not copy the production algorithm
into the expected result.

Match the subject's C dialect, keep `main()` small and make the harness
self-checking. A harness result proves only the host behavior it actually runs;
it does not prove an ARM package, framebuffer, kernel path or phone.

`check c` checks harness formatting. `check python` compiles and runs the current
host harness suite.

## Hardware names and registers

Hardware definitions read from broad owner to specific register or field:

```text
<OWNER>_<BLOCK>_<REGISTER>
<OWNER>_<BLOCK>_<REGISTER>_<FIELD>
```

Examples from the current tree:

```text
UMS9117_LCDC_CTRL
UMS9117_LCDC_CTRL_RUN
UMS9117_MUSB_DMA_CFG
SC2720_RGB_CTRL
SC2720_LDO_USB_PD_REG
SC2720_KPLED_CTRL0_LEVEL_MASK
```

A plain register name is an offset within its mapped block. Use `_PHYS` only for
an absolute physical address. Encoded fields use `_MASK` and `_SHIFT`. Follow
the hardware's established abbreviation when it is part of the register name;
otherwise keep one spelling within the component.

Conventional local helpers such as `ARRAY_SIZE` and `BIT` keep their familiar
spelling when they have exactly the conventional meaning. Project concepts do
not get generic file-scope names such as `RES_*`, `STATE_*` or `TIMEOUT`.

Do not guess register values, ordering, barriers or delays to make a driver or
bootstrap path look complete. The code and its comment must reflect the evidence
level established for that hardware.

## Before sending a C change

- Identify both the execution environment and the owner.
- Prefix cross-file names with their owner and keep file-local names concise.
- Make resource acquisition, cleanup and state restoration visible.
- Check sizes, units, narrowing conversions and arithmetic boundaries.
- Preserve required external names and wire formats.
- Run the checks and builds that compile the changed code in its real context.

If an aport source changed, regenerate its checksum through the
[supported build workflow](../../guides/BUILDING.md#regenerate-alpine-checksums)
before the final gate. The [identity contract](../IDENTITY.md) covers public
device names, the [logging contract](../LOGGING.md) covers messages, and the
[porting overview](../../porting/README.md) defines project, platform and target
ownership.
