# Host-to-phone transfer

Current targets expose an interactive shell, one-off commands, and file copy
through their session-bound SSH/SFTP transport. Load the target first, then use
the same public CLI from the source checkout:

```sh
./fplinux console <target>
```

After the session becomes ready, the same bound endpoint is also available to
ordinary OpenSSH:

```sh
ssh -F "$XDG_RUNTIME_DIR/fplinux/current/<target>.ssh-config" fplinux
```

The private config is available only while that RAM session is ready.

Target documents state the board-specific boot sequence and hardware
limitations.

## Run a command

```sh
./fplinux console <target> --exec 'uname -r'
```

The command runs on the phone and returns its output and exit status to the host.
Use it for small administrative actions or to prepare a destination directory.

## Send a file

```sh
./fplinux console <target> \
  --upload ./module.ko /tmp/module.ko
```

The phone destination must be a safe absolute file path in an existing
directory. The transfer succeeds only when the received bytes match the
host-computed SHA-256 digest; the completed file replaces the destination
atomically. A failed upload does not replace the destination.

The destination filesystem supplies the size limit. On RAM-only targets, a file
placed in a memory-backed directory consumes the phone's RAM. Use persistent or
removable storage only when the target document says it is supported and
mounted.

## Pull a file

```sh
./fplinux console <target> \
  --pull /tmp/capture.raw ./capture.raw
```

The source path follows the same safe absolute-path rule. The host accepts the
file only when its final SHA-256 digest matches the phone's reported digest; an
interrupted or failed pull does not replace the requested local destination.

## Concurrent use

An SSH target can accept another command or transfer while an interactive shell
is open. If a target supports host-keyboard forwarding, it uses a separate input
channel and can run alongside the shell; see that target's document.

Keyboard forwarding grabs the selected input device, so keys from that keyboard
do not reach the host desktop or stop the forwarding process. On a host with no
second keyboard, put a time limit on the client:

```sh
sudo timeout 60s ./fplinux console <target> --keyboard /dev/input/eventN
```

When the timeout expires, the client releases the keyboard.

## Transport limitations

- The private USB link supplies no internet access, DNS or forwarding.
- The public file-copy operations provide no resume or raw block-device mode.
- Interrupted transfers must be started again.
- A transferred kernel module must match the running kernel and is part of the
  current volatile RAM-loaded run.

For the physical connection and archive use, see [Release archives](RELEASES.md).
