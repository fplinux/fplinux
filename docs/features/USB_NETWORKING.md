# USB networking

After a normal FPLinux boot, the phone exposes one private USB-NCM Ethernet
link. The host receives its address by DHCP; the phone supplies the other
address. The runner identifies the link by the exact RAM session, so do not set
addresses manually or reuse an address from an earlier run.

The link is for FPLinux itself:

- [SSH sessions](SSH.md), including SFTP used by [file transfer](FILE_TRANSFER.md);
- the separate generic-serial channel used by the [host keyboard bridge](HOST_KEYBOARD.md).

It is not an Internet connection. The phone starts with IPv4 forwarding off and
no default route. FPLinux provides no gateway, DNS service, route forwarding or
general USB tethering. USB device mode is not USB host or OTG; the target
phone page states whether any board-specific USB capability is available.

## Connection lifecycle

The runner sets up this private link while loading the selected image. A source
checkout uses `./fplinux run <target>`; a standalone archive uses
`./runner/run.py`. Follow the source checkout's loading guide or the standalone
archive's top-level `README.txt` for the loader-first procedure and host USB
permissions.

Linux keeps running when the USB cable is unplugged. Reconnect the cable, then
use the appropriate command in [SSH sessions](SSH.md); do not load a second
image just to restore the link. A power-off, fresh RAM load or different image
creates a different session and therefore a different private link.

The target document is the authority for whether USB networking has been
exercised on that exact phone. This page describes the common transport only.
