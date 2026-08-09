# Moving files between the host and the phone

The TA-1618 exposes two non-ACM serial interfaces under USB ID `0525:a4a6`.
Interface 0 maps to `/dev/ttyGS0` and carries the login shell, interactive
console, `--exec`, `--upload` and `--pull`. Interface 1 maps to `/dev/ttyGS1`
and carries host keyboard events for `fplinux-input`.

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

The destination must be one direct `/tmp/FILE` path. The file is installed only
after the phone has computed its SHA-256 and found it equal to the one the host
computed before sending, and the install is a rename over a temporary file, so
a failed transfer leaves nothing behind. The ceiling is 8 MiB, which is a
memory-safety limit rather than a protocol one.

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

The source must be an absolute path made of letters, digits, `.`, `_` and `-`.
That restriction is what makes single quotes around it sufficient in the shell
line, and it is enforced on the host before the phone is asked anything.

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

Upload waits for an acknowledgement after each window of sixteen base64
lines, while pull requests 32 KiB at a time. The window travels as one USB
transfer. At thirty-two lines the phone stops consuming input, so it remains at
sixteen.

## What the channel imposes

One process can claim each USB interface. A shell or transfer client on
interface 0 can run alongside the keyboard forwarder on interface 1, but two
processes cannot claim the same interface. Close an interactive interface 0
session before running `--exec`, `--upload` or `--pull`.

BusyBox `getty` puts the line into canonical mode with echo and with software
flow control. Both transfer directions work in text mode for that reason: the
payload travels as base64 lines, which survive a line discipline that would
otherwise eat control bytes. This costs a third of the bandwidth and buys
transfers that need no device-side component and work against any image already
running.

The phone has 62 MiB of RAM with `/tmp` on tmpfs, so the 8 MiB ceiling is not
arbitrary. There is no `/lib/modules`, which is why a driver under test is sent
over this path on every rebuild.

## What is deliberately not here

- **Compression.** Transfer modes preserve the file bytes and do not transform
  payloads. Compression has not been qualified as part of the text-mode protocol.
- **A raw block mode.** Turning off the line discipline would remove the base64
  overhead, but it needs a proven 8-bit transparent path first, and every raw
  excursion has to restore the terminal on each exit path including a crash.
- **Resume.** A pull that is interrupted starts again. The blocks are already
  ranged, so resuming is a matter of sending the digest of the accepted prefix
  and continuing from that offset.
- **A file-transfer helper.** A framed binary protocol would separate command
  output from the kernel log structurally instead of by marker scanning and
  would lift the ceiling to what the endpoint can carry. The installed
  `fplinux-input` helper handles keyboard events only.
- **USB networking.** The second serial interface is reserved for input events.
  The kernel carries no network stack or USB ethernet function.

## The microSD card

For volumes this channel is wrong for, the card is the way past it: the host
reads at 19.6 MiB/s and writes at 8.73 MiB/s on the card those figures were
measured with, an order of magnitude beyond what the console carries.

A file reaches the card through the console rather than around it. Sending
writes into `/tmp`, the shell moves it onto the mount point, and reading takes
any absolute path, so the whole way there and back is:

```sh
./fplinux console nokia-ta1618 --upload ./file.bin /tmp/file.bin
./fplinux console nokia-ta1618 \
  --exec 'mv /tmp/file.bin /mnt/card/file.bin && sync'
./fplinux console nokia-ta1618 --pull /mnt/card/file.bin ./file.bin
```

Four mebibytes survive that round trip byte for byte, including an unmount and
a remount in the middle, and reading off the card runs at the same rate as
reading out of RAM. The card is not the limit in this path; the console is.

Carrying the card between machines is the only way past that, and it is a
manual step. The card host also claims the ADI controller and analog slave
windows for itself rather than sharing them under the hardware lock the way the
keypad does, so no second driver needing that transport can be bound while the
card host is.
