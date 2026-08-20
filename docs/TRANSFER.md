# Moving files between the host and the phone

Current FPLinux console targets expose two non-ACM serial interfaces under USB
ID `0525:a4a6`. Interface 0 maps to `/dev/ttyGS0` and carries the login shell,
interactive console, `--exec`, `--upload` and `--pull`. Interface 1 maps to
`/dev/ttyGS1` and carries host keyboard events for `fplinux-input`.

`fplinux-usb-console` selects interface 0 automatically for its shell and
transfer modes. `--keyboard` selects interface 1 unless `--interface` overrides
it. A host keyboard forwarder can therefore stay connected while interface 0
is used independently. Commands below run from the repository root through the
single `./fplinux` entrypoint.

## Running a command

```sh
./fplinux console nokia-ta1618 --exec 'uname -r'
```

The command runs in a subshell on the phone. Its standard output becomes this
program's standard output, with the carriage returns the line discipline adds
removed, and its exit status becomes this program's exit status. A command that
calls `exit` ends the subshell rather than the login shell.

Everything the phone says outside the command, the kernel log included, stays
out of standard output: the reply is delimited by markers carrying a nonce, and
only what falls between them is passed on.

## Sending a file

```sh
./fplinux console nokia-ta1618 \
  --upload ./module.ko /tmp/module.ko
```

The destination is a safe absolute file path. Each path component may contain
letters, digits, `.`, `_` and `-`; empty components, `.` and `..` components,
a trailing slash and paths longer than 200 characters are rejected. The target
directory must already exist. Before accepting data, the phone checks that its
filesystem reports enough free space for the file plus 64 KiB.

The phone creates the temporary file in the destination directory, computes its
SHA-256 after receiving the complete payload and compares it with the digest the
host computed before sending. A matching file is installed with an atomic rename.
There is no fixed host-side size ceiling; the destination filesystem provides the
capacity limit. A failed transfer removes its temporary file.

A loadable driver must be built against the exact running kernel. The runtime
has module loading and unloading enabled but no dependency database, so load and
remove a transferred module explicitly:

```sh
./fplinux console nokia-ta1618 --exec 'insmod /tmp/module.ko'
./fplinux console nokia-ta1618 --exec 'rmmod module_name'
```

The module and every state change it makes remain part of the volatile RAM
session.

## Taking a file

```sh
./fplinux console nokia-ta1618 \
  --pull /tmp/capture.raw ./capture.raw
```

The phone reports the size and the digest of the whole file before the first
byte moves. The file then arrives in 32 KiB blocks, each carrying the digest
the phone computed for it; a block whose digest does not match what arrived is
asked for again, up to three times, before the transfer fails by name. The host
writes to `LOCAL.part` and renames only when the assembled file matches the
digest the phone reported at the start. A failed or interrupted pull leaves no
partial file.

The source must follow the same safe absolute-path policy as an upload
destination. That restriction makes single quotes around it sufficient in the
shell line, and it is enforced on the host before the phone is asked anything.
There is no fixed host-side pull limit.

## Measured rates

Taken on a TA-1618 with the hardware-validated damage-driven display driver and
a static framebuffer:

| Direction  | Payload | Time    | Rate       |
| ---------- | ------- | ------- | ---------- |
| `--upload` | 4 MiB   | 5441 ms | 753 KiB/s  |
| `--pull`   | 4 MiB   | 2866 ms | 1.40 MiB/s |

With a settled framebuffer, the LCDC stops and produces no display interrupts; a
static-screen hardware sample measured 99.97% CPU idle. Framebuffer changes
schedule a full frame and complete through the LCDC interrupt, while updates
arriving during a transfer are coalesced into the next frame.

Upload is split into bounded chunks of at most 256 KiB. The phone decodes and
acknowledges each chunk before the host sends the next one. Within a chunk the
host waits after every sixteen base64 lines so it cannot overrun the gadget TTY.
The shell therefore never retains the complete encoded payload in memory. Pull
requests 32 KiB at a time.

## What the channel imposes

One process can claim each USB interface. A shell or transfer client on
interface 0 can run alongside the keyboard forwarder on interface 1, but two
processes cannot claim the same interface. Close an interactive interface 0
session before running `--exec`, `--upload` or `--pull`.

BusyBox `getty` puts the line into canonical mode with echo and software flow
control. Both transfer directions use text mode: payload travels as base64 lines,
which avoid line-discipline interpretation of control bytes. Base64 adds one
third of payload overhead. The transfer protocol uses only the shell and standard
utilities already present in the image.

On the TA-1618, Linux has about 62 MiB of RAM and `/tmp` is backed by tmpfs. An
upload to `/tmp` therefore consumes RAM, while an upload to an already mounted
filesystem consumes that filesystem's free space. There is no `/lib/modules`,
so a loadable driver under test is transferred to `/tmp` for the active RAM
session.

## Unsupported transfer features

- **Compression:** transfer modes preserve file bytes and do not transform
  payloads.
- **Raw block transfer:** interface 0 uses the canonical text-mode shell channel
  and base64 payload lines.
- **Resume:** an interrupted pull starts from the beginning.
- **Dedicated transfer daemon:** no file-transfer helper is installed; framing
  and integrity checks run through the shell protocol. `fplinux-input` handles
  keyboard events only.
- **USB networking:** interface 1 is reserved for input events, and the kernel
  has no network stack or USB Ethernet function.

## TA-1618 microSD card

The INOI targets do not expose microSD storage. On the TA-1618, large files are
better moved through the removable card when possible: the tested card reads at
19.6 MiB/s and writes at 8.73 MiB/s on the host, an order of magnitude beyond
what the console carries.

A file reaches an already mounted card directly through the console. The upload
temporary and final file stay on the same filesystem:

```sh
./fplinux console nokia-ta1618 \
  --exec 'mkdir -p /mnt/card/fplinux/data'
./fplinux console nokia-ta1618 \
  --upload ./file.bin /mnt/card/fplinux/data/file.bin
./fplinux console nokia-ta1618 \
  --exec 'sync'
./fplinux console nokia-ta1618 \
  --pull /mnt/card/fplinux/data/file.bin ./file.bin
```

The card is not the throughput limit in this path; the text console is.

Carrying the card between machines remains the fastest manual path. The card host
reaches its PMIC rails through the shared UMS9117 ADI provider. The provider owns
the controller and analog-slave mappings and serializes MMC, framebuffer, keypad
and power-off transactions under the ADI hardware user lock.
