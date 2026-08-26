# File transfer

FPLinux transfers ordinary files over the current session's SFTP channel. Use
these commands instead of an ad-hoc SFTP invocation when the result matters:
they verify the complete file with SHA-256 and publish it only after that check.

Start or reconnect first with [SSH sessions](SSH.md).

## Upload

```sh
# Source checkout
./fplinux console <target> --upload ./local.bin /tmp/remote.bin

# Standalone archive
./runner/run.py --reconnect --upload ./local.bin /tmp/remote.bin
```

The source must be a regular local file. The destination must be an absolute,
safe file path in an existing directory with enough free space. FPLinux uploads
to a temporary file in that directory, checks both size and SHA-256 on the
phone, then atomically replaces the requested destination. If any step fails,
the requested destination is left unchanged.

## Pull

```sh
# Source checkout
./fplinux console <target> --pull /tmp/remote.bin ./local.bin

# Standalone archive
./runner/run.py --reconnect --pull /tmp/remote.bin ./local.bin
```

The phone source must be a readable regular file at a safe absolute path. The
host records its size and SHA-256, downloads to a temporary file beside the
requested destination, verifies the bytes, checks that the phone source did not
change during the transfer, and then atomically publishes the local file. The
destination directory must already exist.

## Limits

- Transfers do not resume. Start them again after an interruption.
- They operate on files, not directories or raw block devices.
- Space limits come from the destination filesystem. A RAM-backed destination
  consumes the phone's limited RAM.
- Use removable or persistent storage only when the selected target document
  explicitly supports and mounts it.

The private USB link itself has no Internet, DNS or forwarding; see
[USB networking](USB_NETWORKING.md). A copied kernel module must match the
running kernel and is lost with the current RAM session.
