# C code

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
| `tests/*.c`                                                    | Host-only harness                  | The dialect selected by the test that compiles it                     |

Do not move an API or language assumption from one row to another. `errno`, file
descriptors and signals do not belong in the bootstrap. Kernel code does not use
libc types or POSIX calls. A successful host compile does not prove that ARM,
musl, bootstrap or kernel code builds.

## Run the check that understands the layer

```sh
./fplinux check c --no-cache
./fplinux check kernel --no-cache
./fplinux check python --no-cache
./fplinux build <target>
```

These commands establish different things:

- `check c` formats bootstrap, phone userspace, host C, embedded adapter files
  and C test harnesses. Its Clang analysis covers independently compilable phone
  userspace and host translation units only.
- Bootstrap files are formatted by `check c`, but their real compile proof is an
  affected target build.
- Embedded TyrQuake and MicroPython files are formatted by `check c`, but they
  are compiled only in their pinned upstream package builds. C fragments inside
  patch files are judged by that destination build, not by standalone analysis.
- `check kernel` projects code into the pinned Linux tree and runs its formatter,
  checkpatch, configuration, Devicetree and Sparse checks.
- `check python` compiles and runs the host C harnesses driven by the unit suite.
  That result remains host-only evidence.

Run the complete `./fplinux check --no-cache` before submitting a change. Build
every target affected by bootstrap, package, host-tool or kernel integration.

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

Useful cross-file names from the current tree include:

```text
fplinux_boot_screen_render()
fplinux_multitap_press()
ums9117_bootstrap_personalize_dtb()
ums9117_adi_begin()
ta1618_kpled_probe()
```

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

Repository-local header guards are non-reserved and owner-qualified:

```text
FPLINUX_BOOT_SCREEN_H
FPLINUX_MULTITAP_H
FPLINUX_UMS9117_FB_INTERNAL_H
```

Avoid guards beginning with `__` or an underscore followed by an uppercase
letter. A public header includes the declarations needed for every type it
exposes instead of depending on include order in one consumer.

## Kernel code

Follow the Linux coding style and the API of the subsystem being changed. Use
kernel types, negative errno values, `devm_*` where its lifetime matches, and
`dev_err_probe()` for probe errors that may defer. Keep logging severity and
message shape consistent across the kernel layer.

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
command output and exit behavior stable. Keep message prefixes and severity
consistent across phone userspace.

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

`check c` formats harnesses. `check python` compiles and runs the current host
harness suite.

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
- Keep cross-file names qualified and file-local names concise.
- Make resource acquisition, cleanup and state restoration visible.
- Check sizes, units, narrowing conversions and arithmetic boundaries.
- Preserve required external names and wire formats.
- Run the checks and builds that compile the changed code in its real context.

If an aport source changed, regenerate its checksum with `./fplinux checksum
<aport>` before the final gate. The
[porting overview](../porting/README.md) defines project, platform and target
ownership.
