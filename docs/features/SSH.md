# SSH sessions

FPLinux opens an SSH and SFTP service only for the private USB session created
by its runner. It is a root shell authenticated with a fresh session key; there
is no password login, port forwarding, agent forwarding or X11 forwarding.

Start a RAM session first with the source checkout's loading guide or the
standalone archive's top-level `README.txt`. Once it is ready, open a shell with
the command that matches how the image was started:

```sh
# Source checkout
./fplinux console <target>

# Standalone archive
./runner/run.py --reconnect
```

Leaving the shell or using the OpenSSH escape `~.` disconnects the host. It
does not stop Linux. After a physical USB replug, run the same reconnect command
again.

## Run one command

Use `--exec` when an interactive shell is unnecessary. The command runs on the
phone; its output and exit status are returned to the host.

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
