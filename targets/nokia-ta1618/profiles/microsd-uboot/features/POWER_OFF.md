# Power-off with the microSD system card

This page applies to the Nokia 3210 4G (TA-1618) `microsd` boot mode.

1. Exit applications and the host shell.
2. Disconnect USB and make sure no charger is attached.
3. Hold the red handset key continuously for five seconds.
4. Wait until the phone is fully off before removing the microSD card.

The five-second hold requests an orderly system shutdown. OpenRC stops running
services, flushes pending writes and remounts the system card read-only before
the phone powers off. If userspace shutdown cannot start, the phone remains on
instead of forcing power off with a writable root filesystem.

A short press remains an input event, releasing the key early cancels the
request, and detected external charger input refuses shutdown.

Do not try to unmount the root partition and do not remove the card from a
running system. Linux reboot and suspend are not supported substitutes for
power-off. If shutdown does not complete, remove and reinsert the battery before
booting again; treat the card as unclean until it has been checked.
