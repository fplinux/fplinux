# Moving files between the host and the phone

The phone reaches the host through one channel and nothing else: a single bulk
endpoint pair carrying a shell. There is no network in the kernel, not even
local sockets, and the USB controller declares one pair of endpoints, so every
transfer described here rides the same console the operator types into.

`fplinux-usb-console` therefore carries three non-interactive modes besides the
terminal it opens by default. All three take the same device selection options
as interactive use.

## Running a command

```sh
fplinux-usb-console --exec 'uname -r'
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
fplinux-usb-console --upload ./module.ko /tmp/module.ko
```

The destination must be one direct `/tmp/FILE` path. The file is installed only
after the phone has computed its SHA-256 and found it equal to the one the host
computed before sending, and the install is a rename over a temporary file, so
a failed transfer leaves nothing behind. The ceiling is 8 MiB, which is a
memory-safety limit rather than a protocol one.

## Taking a file

```sh
fplinux-usb-console --pull /tmp/capture.raw ./capture.raw
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

Taken on a TA-1618 over the USB console:

| Direction  | Payload | Time    | Rate       |
| ---------- | ------- | ------- | ---------- |
| `--upload` | 27 KiB  | 75 ms   | 356 KiB/s  |
| `--upload` | 4 MiB   | 5867 ms | 698 KiB/s  |
| `--pull`   | 1 MiB   | 731 ms  | 1.43 MiB/s |
| `--pull`   | 4 MiB   | 2846 ms | 1.41 MiB/s |

The setup cost of a transfer is fixed, so the sending rate depends on how much
there is to send. Reading is already at its ceiling by one mebibyte.

Sending is the slower direction because it waits: a window of sixteen base64
lines, 912 bytes of payload, is acknowledged before the next window goes out,
where reading asks for 32 KiB at a time. The window is not free to grow. At
twenty-four lines the phone still keeps up and the transfer gains five percent;
at thirty-two it stops consuming and the transfer fails. Sixteen is what the
line discipline tolerates with margin, and the window travels as one USB
transfer rather than sixteen.

For scale, the whole of the phone's RAM is 62 MiB, and the 8 MiB a transfer is
allowed to move crosses in six seconds one way and twelve the other.

## What the channel imposes

Only one host process can hold the phone, because the client claims the usbfs
interface and the runner ends by replacing itself with that client. An
interactive session must be closed before a transfer mode can run.

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

- **Compression.** The phone has one ARMv7 core, and it compresses with `gzip`
  at 1.0 MiB/s while decompressing at 7.5 MiB/s, at a ratio of 1.85 on a real
  binary. Compressing is therefore slower than the pull channel carries the
  bytes uncompressed, and a 1.75 MiB file that arrives in 1.28 s takes 2.52 s
  if the phone squeezes it first. The other direction does gain, because the
  host compresses and the phone only decompresses, but the payload that travels
  that way is a kernel module of a few tens of kilobytes and the saving is
  around twenty milliseconds against a rebuild that takes minutes.
- **A raw block mode.** Turning off the line discipline would remove the base64
  overhead, but it needs a proven 8-bit transparent path first, and every raw
  excursion has to restore the terminal on each exit path including a crash.
- **Resume.** A pull that is interrupted starts again. The blocks are already
  ranged, so resuming is a matter of sending the digest of the accepted prefix
  and continuing from that offset.
- **A device-side helper.** A framed binary protocol would separate command
  output from the kernel log structurally instead of by marker scanning, and
  would lift the ceiling to what the endpoint can carry. It costs a new runtime
  component and a rebuild to reach a phone.
- **A second USB gadget function.** The highest ceiling and the highest cost:
  the FIFO table declares one bulk pair, `CONFIG_USB_CONFIGFS` is unset, the USB
  identity is fixed in the target manifest and in the udev rule, and the
  ethernet variant would need the whole network stack in a 62 MiB system.

## The microSD card

For volumes this channel is wrong for, the card is the way past it: the host
reads at 19.6 MiB/s and writes at 8.73 MiB/s on the card those figures were
measured with, an order of magnitude beyond what the console carries.

A file reaches the card through the console rather than around it. Sending
writes into `/tmp`, the shell moves it onto the mount point, and reading takes
any absolute path, so the whole way there and back is:

```sh
fplinux-usb-console --upload ./file.bin /tmp/file.bin
fplinux-usb-console --exec 'mv /tmp/file.bin /mnt/card/file.bin && sync'
fplinux-usb-console --pull /mnt/card/file.bin ./file.bin
```

Four mebibytes survive that round trip byte for byte, including an unmount and
a remount in the middle, and reading off the card runs at the same rate as
reading out of RAM. The card is not the limit in this path; the console is.

Carrying the card between machines is the only way past that, and it is a
manual step. The card host also claims the ADI controller and analog slave
windows for itself rather than sharing them under the hardware lock the way the
keypad does, so no second driver needing that transport can be bound while the
card host is.
