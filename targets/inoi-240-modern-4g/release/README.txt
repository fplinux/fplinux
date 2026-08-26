FPLinux for INOI 240 Modern 4G

Read docs/guides/STANDALONE.md before connecting the phone. It contains the
host requirements, USB setup, checksum, loader-first and reconnect procedures.
This target's BootROM key is * (asterisk); hold it when the loader asks for the
powered-off phone.

Current target support:
  - local 128x160 terminal and physical keypad;
  - USB SSH/SFTP and host-keyboard forwarding;
  - installable TyrQuake;
  - installable MicroPythonOS navigation and keypad text input, with the
    128x160 layout limitation described below.

microSD, internal phone storage, audio, modem, Bluetooth, Wi-Fi and Linux
power-off are not supported by this target.

This target has no supported microSD path. TyrQuake therefore keeps game data
in tmpfs, where it consumes the phone's limited RAM. MicroPythonOS state also
remains in RAM. Its 128x160 UI is only partially adaptive: launcher content and
some application controls are clipped.

To end the RAM-only session, disconnect USB, remove and reinsert the battery,
then boot the phone normally.
