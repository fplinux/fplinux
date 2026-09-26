# SSH sessions

FPLinux opens an SSH and SFTP service only for the private USB session created
by its runner. It is a root shell authenticated with a fresh session key; there
is no password login, port forwarding, agent forwarding or X11 forwarding.

Start a session first by following
[Loading from a source checkout](../guides/LOADING.md) or
[Using a standalone archive](../guides/STANDALONE.md). Once it is ready, open a
shell with the command that matches how the image was started:

```sh
# Source checkout, default profile
./fplinux console <target>

# Source checkout, microSD system root
./fplinux console <target> --profile microsd-uboot

# Standalone archive
./runner/run.py --reconnect
```

A source-checkout command uses one profile's build: `default` unless
`--profile` names another. For a session started with
`--profile microsd-uboot` or `--boot microsd`, add `--profile microsd-uboot` to
every `./fplinux console` and `./fplinux verify` command. Without it, the shell,
`--exec`, `--upload`, `--pull` and `verify` refuse the session because the
running image is not the default profile's build.

Leaving the shell or using the OpenSSH escape `~.` closes that shell without
stopping Linux. Session checks and file transfers reuse an authenticated
connection for the same RAM session; it closes after 60 seconds with no active
channels. Starting a new RAM session closes the previous session's connection.

After a physical USB replug, run the same reconnect command again. FPLinux checks
the phone's session and image identity again before running the requested
operation. Interrupted commands and transfers are not automatically repeated.

## Run one command

Use `--exec` when an interactive shell is unnecessary. The command runs on the
phone; its output and exit status are returned to the host.

Each `--exec` command and raw NAND download uses a separate connection. This
keeps cancellation of its remote process independent of other open channels.

```sh
# Source checkout
./fplinux console <target> --exec 'uname -r'

# Standalone archive
./runner/run.py --reconnect --exec 'uname -r'
```

## Ordinary OpenSSH

After a source-checkout session becomes ready, its private OpenSSH configuration
is available only for that session:

```sh
ssh -F "$XDG_RUNTIME_DIR/fplinux/current/<target>.ssh-config" fplinux
```

The configuration pins the session host key and client key. Do not copy it to a
later session or use it as a general-purpose phone login. The source-checkout
and archive commands above are the supported ways to reconnect because they
check that the requested target and current image still match the session.

For verified uploads and downloads, use [file transfer](FILE_TRANSFER.md).
See the selected phone page for its USB support and end-of-session procedure.
