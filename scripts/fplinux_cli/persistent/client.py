# SPDX-License-Identifier: GPL-2.0-only
"""Host-side download client for the BootROM and loader stages.

The command flow is the one FPLinux's launcher feeds spd_dump today
(``fdl t117_fdl1.bin 0x6200``, then ``fdl <stage> 0x80100000``) plus
the flash stage's commands this design adds. The transport is any
object with ``transfer(bytes) -> bytes``: the in-process simulator
for tests and the bundled USB layer on real hardware.

Phase order, as spd_dump.c drives it:

1. BROM handshake: vendor "start download" request, one ``0x7e``,
   BSL_REP_VER (``SPRD3`` marks an open BootROM), CONNECT.
2. BROM download: START_DATA/MIDST_DATA/END_DATA, one image, then
   EXEC_DATA, or for a target with an exec distance the workaround
   frame of t117_exec_workaround.
3. FDL1 handshake: four ``0x7e`` (retried), BSL_REP_VER parsed for
   ``CHIP ID``, CONNECT; frames switch to the word-sum checksum.
4. Stage download: same transfer commands, EXEC skips the DHTB
   header on the device side.
5. Flash stage: READ_FLASH, ENABLE_WRITE_FLASH, ERASE_FLASH and the
   start/midst/end program transaction for the raw boot regions.
"""

from __future__ import annotations

import struct
from collections.abc import Callable

from .frames import (
    MODE_CRC16,
    MODE_SUM16,
    FrameReader,
    encode_check_baud,
    encode_packet,
)
from .nandsim import (
    BSL_CMD_CONNECT,
    BSL_CMD_ENABLE_WRITE_FLASH,
    BSL_CMD_END_DATA,
    BSL_CMD_ERASE_FLASH,
    BSL_CMD_EXEC_DATA,
    BSL_CMD_MIDST_DATA,
    BSL_CMD_READ_FLASH,
    BSL_CMD_START_DATA,
    BSL_REP_ACK,
    BSL_REP_READ_FLASH,
    BSL_REP_VER,
    UMS9117_CHIP_ID,
    exec_distance_trailing,
    expected_exec_distance_pad,
)

USB_VENDOR_DOWNLOAD_REQUEST = (0x21, 34, 0x0601)


class ProtocolError(Exception):
    """The device answered with an unexpected response."""


class DownloadClient:
    """One synchronous host session with a BootROM or loader stage."""

    def __init__(self, transport: Callable[[bytes], bytes]) -> None:
        self.transport = transport
        self.mode = MODE_CRC16
        self.reader = FrameReader(mode=MODE_CRC16)
        self.secure_boot = False
        self.chip_id = 0
        self.verbose: Callable[[str], None] | None = print

    def _log(self, message: str) -> None:
        if self.verbose is not None:
            self.verbose(message)

    def _send_raw(self, blob: bytes) -> None:
        reply = self.transport(blob)
        self.reader.feed(reply)
        self.reader.mode = self.mode

    def _send(self, message_type: int, payload: bytes = b"") -> tuple[int, bytes]:
        """Send one frame and return the device's single reply."""
        self._send_raw(encode_packet(message_type, payload, mode=self.mode))
        frames = self.reader.frames()
        replies = [frame for frame in frames if frame[0] != 0xFF]
        if not replies:
            raise ProtocolError("device did not answer the command")
        if len(replies) != 1:
            raise ProtocolError(f"device replied with {len(replies)} responses")
        return replies[0]

    def _command(self, message_type: int, payload: bytes = b"") -> tuple[int, bytes]:
        reply_type, reply = self._send(message_type, payload)
        if reply_type == BSL_REP_ACK:
            return reply_type, reply
        raise ProtocolError(f"command 0x{message_type:02x} rejected with 0x{reply_type:02x}")

    # Phase 1: BootROM handshake.

    def vendor_start(self) -> None:
        """Issue the USB "start download" request the BROM expects.

        The libusb control transfer happens in the transport; the
        request constants record what the layered USB transport must
        send.
        """
        self._log(f"vendor request {USB_VENDOR_DOWNLOAD_REQUEST}")

    def brom_handshake(self) -> str:
        """Sync with the BootROM and return its version string."""
        self._send_raw(encode_check_baud(1))
        _sync, frames = None, None
        for _ in range(50):
            frames = self.reader.frames()
            if frames:
                break
        if not frames:
            raise ProtocolError("BootROM did not answer the sync byte")
        reply_type, payload = frames[0]
        if reply_type != BSL_REP_VER:
            raise ProtocolError(f"BootROM replied 0x{reply_type:02x}")
        version = payload.rstrip(b"\x00").decode("ascii", "replace")
        if payload != b"SPRD3\x00":
            self.secure_boot = True
        self._command(BSL_CMD_CONNECT)
        return version

    # Phase 2: BROM download and execution.

    def download(self, blob: bytes, address: int, *, block: int = 528) -> None:
        """Download one image to an address."""
        self._command(
            BSL_CMD_START_DATA,
            struct.pack(">II", address, len(blob)),
        )
        for index in range(0, len(blob), block):
            self._command(BSL_CMD_MIDST_DATA, blob[index : index + block])
        self._command(BSL_CMD_END_DATA)

    def exec_distance(self, address: int, distance: int) -> None:
        """Send the T117 exec-distance workaround frame, without ack.

        Port of t117_exec_workaround: a zero pad whose length comes
        from escaped distance bytes, 252 little-endian repeats of the
        entry address (load address plus 0x200), and one align byte
        for an odd pad. The device does not acknowledge it.
        """
        entry = address + 0x200
        pad = expected_exec_distance_pad(distance, entry)
        payload = bytearray(pad)
        payload += struct.pack("<I", entry) * 252
        payload += bytes(exec_distance_trailing(pad))
        self._send_raw(encode_packet(distance, bytes(payload), mode=self.mode))
        self._log(f"exec-distance distance=0x{distance:x} entry={entry:#x}")

    def exec_download(self, address: int, *, distance: int = 0, expect_ack: bool = True) -> None:
        """Execute the downloaded image, with the distance variant.

        The BootROM acknowledges its EXEC before running the stage; a
        running FDL jumps without an acknowledgement, so stage
        executions pass ``expect_ack=False``.
        """
        if distance:
            self.exec_distance(address, distance)
        elif expect_ack:
            self._command(BSL_CMD_EXEC_DATA)
        else:
            self._send_raw(encode_packet(BSL_CMD_EXEC_DATA, b"", mode=self.mode))
            self._log(f"exec 0x{address:08x} (no ack expected)")

    # Phase 3: the running stage's handshake.

    def stage_handshake(self, *, sync_bytes: int = 4, retries: int = 10) -> str:
        """Switch to the stage checksum and return its version string.

        The stage announces itself as t117_fdl does: scan the version
        out of BSL_REP_VER and pull the chip ID out of its text, which
        identifies the UMS9117 (0x98180000).
        """
        self.mode = MODE_SUM16
        self.reader.mode = MODE_SUM16
        deadline = retries
        reply_type, payload = 0, b""
        for _ in range(max(deadline, 1)):
            self._send_raw(encode_check_baud(sync_bytes))
            frames = self.reader.frames()
            for frame in frames:
                if frame[0] != BSL_REP_VER:
                    continue
                reply_type, payload = frame
                break
            if reply_type == BSL_REP_VER:
                break
        if reply_type != BSL_REP_VER:
            raise ProtocolError("stage did not answer the sync burst")
        version = payload.rstrip(b"\x00").decode("ascii", "replace")
        marker = "CHIP ID = 0x"
        if marker in version:
            self.chip_id = int(version.split(marker)[1].split()[0], 16)
        self._check_chip()
        self._command(BSL_CMD_CONNECT)
        return version

    def _check_chip(self) -> None:
        if self.chip_id and (self.chip_id ^ UMS9117_CHIP_ID) >> 16 != 0:
            raise ProtocolError(f"device reports an unexpected chip: {self.chip_id:#010x}")

    # Phase 5: flash-region requests.

    def read_flash(self, region: int, offset: int, size: int, *, block: int = 1024) -> bytes:
        """Read ``size`` bytes of a raw flash region at ``offset``."""
        chunk = bytearray()
        cursor = offset
        end = offset + size
        while cursor < end:
            count = min(end - cursor, block)
            reply_type, payload = self._send(
                BSL_CMD_READ_FLASH,
                struct.pack(">III", region, count, cursor),
            )
            if reply_type != BSL_REP_READ_FLASH:
                raise ProtocolError(f"read rejected with 0x{reply_type:02x}")
            if len(payload) != count:
                raise ProtocolError(f"short read: {len(payload)} of {count}")
            chunk += payload
            cursor += count
        return bytes(chunk)

    def enable_write(self) -> None:
        self._command(BSL_CMD_ENABLE_WRITE_FLASH)

    def erase_flash(self, region: int, size: int) -> None:
        self._command(BSL_CMD_ERASE_FLASH, struct.pack(">II", region, size))

    def program_region(self, region: int, data: bytes, *, block: int = 1024) -> None:
        """Erase-then-program one flash region through the commit path."""
        self.enable_write()
        self._command(BSL_CMD_START_DATA, struct.pack(">II", region, len(data)))
        for index in range(0, len(data), block):
            self._command(BSL_CMD_MIDST_DATA, data[index : index + block])
        self._command(BSL_CMD_END_DATA)


def connect_run_device(  # noqa: ANN201 -- test helper type kept local
    device,
    fdl1_image: bytes,
    stage_image: bytes,
    *,
    fdl1_address: int = 0x6200,
    stage_address: int = 0x80100000,
    exec_distance: int = 0,
    disconnect: Callable[[DownloadClient], None] | None = None,
) -> DownloadClient:
    """Run the loader flow against one device and return the client.

    This mirrors the launcher's spd_dump argument list for one target:
    the pinned FDL1 at 0x6200 and the FPLinux stage at 0x80100000,
    with the target's exec distance applied to the first execution.
    """
    client = DownloadClient(device.transfer)
    client.vendor_start()
    client.brom_handshake()
    client.download(fdl1_image, fdl1_address)
    client.exec_download(fdl1_address, distance=exec_distance)
    client.stage_handshake()
    client.download(stage_image, stage_address)
    # A running FDL jumps to the stage without an acknowledgement.
    client.exec_download(stage_address, expect_ack=False)
    if disconnect is not None:
        disconnect(client)
    return client

