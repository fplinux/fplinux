# SPDX-License-Identifier: GPL-2.0-only
"""Simulated UMS9117 phone: BootROM, loader stages, NAND and power-on.

The simulator is an executable model of the chain the persistent-boot
design depends on. Its BootROM and FDL state machines follow the
behaviour the host tool drives (spd_dump.c) and the phone-side
reference (t117_fdl/main.c, packet.c):

* BROM phase: CRC-16 frames, 0x7e sync burst, ``SPRD3`` version reply,
  one raw download, then EXEC or the T117 exec-distance workaround
  packet that t117_exec_workaround sends (a frame whose type is the
  distance and whose payload repeats the entry address).
* FDL phase: word-sum frames, ``CHIP ID = 0x98180000`` version reply,
  start/midst/end downloads whose EXEC skips the DHTB header.
* Flash stage: READ_FLASH / ERASE_FLASH with the Spreadtrum magic
  region IDs (0x80000001 boot0, boot1, kernel, user), the
  ENABLE_WRITE_FLASH gate in front of every program and erase, and a
  read-back verify inside the program commit.

The NAND model keeps raw pages with OOB and factory bad-block
markers. The power-on walk follows the decision order of the real
phone: download mode when the boot key is held, otherwise boot0/boot1
chain validated through DHTB headers exactly like the stock boot.
"""

from __future__ import annotations

import hashlib
import struct
from collections.abc import Callable

from . import dhtb
from .frames import (
    HDLC_HEADER,
    MODE_CRC16,
    MODE_SUM16,
    FrameReader,
    encode_packet,
)

# BSL command values, from spd_cmd.h and t117_fdl/cmd_def.h.
BSL_CMD_CONNECT = 0x00
BSL_CMD_START_DATA = 0x01
BSL_CMD_MIDST_DATA = 0x02
BSL_CMD_END_DATA = 0x03
BSL_CMD_EXEC_DATA = 0x04
BSL_CMD_READ_FLASH = 0x06
BSL_CMD_ERASE_FLASH = 0x0A
BSL_CMD_ENABLE_WRITE_FLASH = 0x1B
BSL_REP_ACK = 0x80
BSL_REP_VER = 0x81
BSL_REP_INVALID_CMD = 0x82
BSL_REP_UNKNOWN_CMD = 0x83
BSL_REP_OPERATION_FAILED = 0x84
BSL_REP_DOWN_DEST_ERROR = 0x89
BSL_REP_DOWN_SIZE_ERROR = 0x8A
BSL_REP_DOWN_EARLY_END = 0x88
BSL_REP_VERIFY_ERROR = 0x8B
BSL_REP_READ_FLASH = 0x93

# packet.h: MAX_PKT_SIZE is 0x1000, so a READ_FLASH reply payload
# larger than MAX_PKT_PAYLOAD can never be framed back.
MAX_PKT_PAYLOAD = 0x1000 - 8

UMS9117_CHIP_ID = 0x98180000
SDRAM_BASE = 0x80000000
SDRAM_SIZE = 64 << 20
ROM_VERSION = b"SPRD3\x00"

# Magic flash-region addresses of the 4G feature-phone downloaders.
REGION_BOOT0 = 0x80000001
REGION_BOOT1 = 0x80000002
REGION_KERNEL = 0x80000003
REGION_USER = 0x80000004

ExecAction = Callable[["SimulatedDevice"], str]


def _escaped(byte: int) -> bool:
    return byte in (0x7D, 0x7E)


def expected_exec_distance_pad(distance: int, entry: int) -> int:
    """Return the leading zero-pad of a workaround packet.

    Port of t117_exec_workaround in spd_dump.c: skip starts at 3 and
    loses one position for each escaped 0x7d/0x7e byte in the distance
    value. The host refuses entries whose low bytes are escaped, so
    only the distance shapes the pad; exec_distance_trailing gives the
    one align byte that follows the words for an odd pad.
    """
    skip = 3
    if _escaped(distance & 0xFF):
        skip -= 1
    if _escaped((distance >> 8) & 0xFF):
        skip -= 1
    if distance >> 16:
        raise ValueError("exec distance exceeds 16 bit")
    if entry >= 0x40000 or _escaped(entry & 0xFF) or _escaped((entry >> 8) & 0xFF):
        raise ValueError("unsupported exec address for the distance workaround")
    return skip


def exec_distance_trailing(skip: int) -> int:
    """Return the align bytes written after the 252 entry words."""
    return 1 if skip & 1 else 0


class SimulatedNand:
    """Raw NAND with per-page OOB, block erase and factory markers."""

    def __init__(
        self,
        *,
        page_main_bytes: int = 2048,
        oob_bytes: int = 64,
        pages_per_block: int = 64,
        block_count: int = 256,
        bad_blocks: set[int] | None = None,
    ) -> None:
        self.page_main_bytes = page_main_bytes
        self.oob_bytes = oob_bytes
        self.pages_per_block = pages_per_block
        self.block_count = block_count
        self.block_bytes = pages_per_block * page_main_bytes
        self.main = bytearray(block_count * self.block_bytes)
        self.main[:] = b"\xff" * len(self.main)
        self.oob = bytearray(block_count * pages_per_block * oob_bytes)
        self.oob[:] = b"\xff" * len(self.oob)
        for block in sorted(bad_blocks or set()):
            self.mark_factory_bad(block)

    def mark_factory_bad(self, block: int) -> None:
        for page in range(self.pages_per_block):
            index = block * self.pages_per_block + page
            self.oob[index * self.oob_bytes : index * self.oob_bytes + 2] = b"\x00\x00"

    def is_factory_bad(self, block: int) -> bool:
        index = block * self.pages_per_block
        return self.oob[index * self.oob_bytes : index * self.oob_bytes + 2] == b"\x00\x00"

    def erase_block(self, block: int) -> None:
        if block >= self.block_count:
            raise ValueError("block outside the NAND")
        base = block * self.block_bytes
        self.main[base : base + self.block_bytes] = b"\xff" * self.block_bytes
        if self.is_factory_bad(block):
            return
        start = block * self.pages_per_block * self.oob_bytes
        end = start + self.pages_per_block * self.oob_bytes
        self.oob[start:end] = b"\xff" * (end - start)


class FlashRegions:
    """Map downloader magic region IDs onto raw NAND blocks.

    The default profile matches the region layout public T117 flashers
    observe: boot0 at block 0, boot1 after it, then kernel and user.
    A production installer builds this map from the phone's own VBM
    partition table in its saved backup; the simulator accepts any
    profile so the discovery logic stays testable either way.
    """

    STOCK_BLOCKS = ((REGION_BOOT0, 0, 1), (REGION_BOOT1, 1, 4))

    def __init__(self, nand: SimulatedNand, region_blocks: dict[int, tuple[int, int]]) -> None:
        self.nand = nand
        self.regions = dict(region_blocks)

    @classmethod
    def stock_profile(cls, nand: SimulatedNand) -> "FlashRegions":
        regions = {address: (start, count) for address, start, count in cls.STOCK_BLOCKS}
        regions[REGION_KERNEL] = (5, 32)
        regions[REGION_USER] = (37, nand.block_count - 37)
        return cls(nand, regions)

    def region_of(self, address: int) -> tuple[int, int]:
        try:
            return self.regions[address]
        except KeyError:
            raise ValueError(f"unknown flash region: {address:#010x}") from None

    def read(self, address: int, offset: int, size: int) -> bytes:
        first, count = self.region_of(address)
        if offset + size > count * self.nand.block_bytes:
            raise ValueError("read outside the flash region")
        base = first * self.nand.block_bytes
        return bytes(self.nand.main[base + offset : base + offset + size])

    def erase_region_blocks(self, address: int, blocks: int) -> None:
        first, count = self.region_of(address)
        if blocks > count:
            raise ValueError("erase outside the flash region")
        for block in range(first, first + blocks):
            self.nand.erase_block(block)

    def program(self, address: int, data: bytes) -> None:
        """Erase-then-program the region head with a read-back verify."""
        first, count = self.region_of(address)
        capacity = count * self.nand.block_bytes
        if len(data) > capacity:
            raise ValueError("region too small for the image")
        needed = (len(data) + self.nand.block_bytes - 1) // self.nand.block_bytes
        for block in range(first, first + needed):
            if self.nand.is_factory_bad(block):
                raise ValueError("factory-bad block inside the boot region")
            self.nand.erase_block(block)
        base = first * self.nand.block_bytes
        self.nand.main[base : base + len(data)] = data
        if bytes(self.nand.main[base : base + len(data)]) != data:
            raise ValueError("read-back verify failed")


class SimulatedDevice:
    """One UMS9117 phone: BROM, FDL stages, flash stage, NAND, boot.

    The host exchanges bytes with ``transfer``; every stage keeps its
    own FrameReader, and the known stage images are registered with
    ``register_stage_image`` so both the download chain (BROM exec,
    FDL1 exec) and the power-on chain can resolve them by digest.
    """

    def __init__(
        self,
        nand: SimulatedNand | None = None,
        *,
        regions: FlashRegions | None = None,
        exec_distance: int = 0,
    ) -> None:
        self.nand = nand or SimulatedNand()
        self.regions = regions or FlashRegions.stock_profile(self.nand)
        self.exec_distance = exec_distance
        self.exec_distance_done = False
        self.stage: object | None = None
        self.stage_mode = MODE_CRC16
        self.usb_session = False
        self.to_host = bytearray()
        self.reader = FrameReader(mode=MODE_CRC16, role="device")
        self.download: dict[str, int] = {}
        self.download_address = 0
        self.download_buffer = bytearray()
        self.write_enabled = False
        self.sync_seen = False
        self.version_sent = False
        self.chain_log: list[str] = []
        self.exec_handlers: dict[str, ExecAction] = {}
        self.sd_card: dict[str, bytes] | None = None
        self.exec_distance_valid = False

    # Stage registry.

    def register_stage_image(self, blob: bytes, action: ExecAction) -> None:
        """Register an action for a stage image, wrapped or raw.

        DHTB-wrapped images register by their payload digest so the
        power-on walk (which reads the payload out of the flash
        region) and the download path (which sees the whole blob)
        resolve to the same action.
        """
        key = blob
        try:
            image = dhtb.Image(blob)
        except ValueError:
            pass
        else:
            key = image.payload
        self.exec_handlers[hashlib.sha256(key).hexdigest()] = action

    def _action_for(self, blob: bytes) -> ExecAction | None:
        keys = [blob]
        try:
            image = dhtb.Image(blob)
        except ValueError:
            pass
        else:
            keys.append(image.payload)
        for key in keys:
            action = self.exec_handlers.get(hashlib.sha256(key).hexdigest())
            if action is not None:
                return action
        return None

    # Transport.

    def transfer(self, chunk: bytes) -> bytes:
        """Consume host bytes and return the pending replies.

        CHECK_BAUD is a burst of raw 0x7e flags, not a frame: the
        first flag draws one version reply per stage, and the reader's
        idle-flag collapsing keeps the burst from shadowing the next
        frame, exactly like dl_get_packet resync on the phone.
        """
        for byte in chunk:
            if byte == HDLC_HEADER and not self.sync_seen:
                self.sync_seen = True
                self.to_host += encode_packet(
                    BSL_REP_VER, self.stage_version(), mode=self.stage_mode, role="device"
                )
        self.reader.mode = self.stage_mode
        self.reader.feed(chunk)
        while True:
            frames = self.reader.frames()
            if not frames:
                break
            for message_type, payload in frames:
                self.handle_frame(message_type, payload)
        reply = bytes(self.to_host)
        self.to_host.clear()
        return reply

    def stage_version(self) -> bytes:
        """One version banner per stage, as the boot chain prints it."""
        if self.stage == "fdl1":
            return b"Custom FDL1: CHIP ID = 0x%08x\x00" % UMS9117_CHIP_ID
        if self.stage == "flash":
            return b"FPLinux flash stage: CHIP ID = 0x%08x\x00" % UMS9117_CHIP_ID
        return ROM_VERSION

    def switch_stage(self, stage: str) -> None:
        """Enter a stage; the next sync burst gets its version."""
        self.stage = stage
        self.stage_mode = MODE_SUM16 if stage != "brom" else MODE_CRC16
        self.sync_seen = False
        self.write_enabled = False
        # A stage starts with no download open: the packet state of
        # the stage before the jump must not survive it, exactly like
        # the freshly initialised state of flash_stage_main() in the
        # C sketch of the reference's dl_main().
        self.download: dict[str, int] = {}
        self.download_buffer = bytearray()
        self.reader = FrameReader(mode=self.stage_mode, role="device")

    def reply(self, message_type: int, payload: bytes) -> None:
        self.to_host += encode_packet(message_type, payload, mode=self.stage_mode, role="device")

    # Command handling, one table for all stages.

    def handle_frame(self, message_type: int, payload: bytes) -> None:
        if message_type == BSL_CMD_CONNECT:
            self.reply(BSL_REP_ACK, b"")
            return
        if message_type == BSL_CMD_START_DATA:
            if len(payload) != 8:
                # t117_fdl data_start: a known command with a wrong
                # length is INVALID_CMD, never UNKNOWN_CMD.
                self.reply(BSL_REP_INVALID_CMD, b"")
                return
            address, size = struct.unpack(">II", payload)
            if self.stage == "fdl1" and not SDRAM_BASE <= address < SDRAM_BASE + SDRAM_SIZE:
                self.reply(BSL_REP_DOWN_DEST_ERROR, b"")
                return
            if self.stage == "flash" and not self.write_enabled:
                self.reply(BSL_REP_VERIFY_ERROR, b"")
                return
            if self.stage == "flash" and address not in (
                REGION_BOOT0,
                REGION_BOOT1,
            ):
                # Kernel and user are read-only by construction in the
                # flash stage, exactly like the C sketch's START_DATA.
                self.reply(BSL_REP_DOWN_DEST_ERROR, b"")
                return
            self.download_address = address
            self.download_buffer = bytearray()
            self.download["address"] = address
            self.download["size"] = size
            self.reply(BSL_REP_ACK, b"")
            return
        if message_type == BSL_CMD_MIDST_DATA:
            remaining = self.download.get("size", 0) - len(self.download_buffer)
            if len(payload) > remaining:
                # t117_fdl data_midst answers an over-long chunk with
                # DOWN_SIZE_ERROR, covering both overrun and no-start.
                self.reply(BSL_REP_DOWN_SIZE_ERROR, b"")
                return
            self.download_buffer += payload
            self.reply(BSL_REP_ACK, b"")
            return
        if message_type == BSL_CMD_END_DATA:
            if len(self.download_buffer) != self.download.get("size", -1):
                self.reply(BSL_REP_DOWN_EARLY_END, b"")
                return
            if self.stage == "flash":
                self.commit_program()
                return
            self.reply(BSL_REP_ACK, b"")
            return
        if self.exec_distance and message_type == self.exec_distance and self.stage == "brom":
            self._exec_distance_workaround(payload)
            return
        if message_type == BSL_CMD_EXEC_DATA:
            if self.exec_distance and not self.exec_distance_done and self.stage == "brom":
                self.reply(BSL_REP_VERIFY_ERROR, b"")
                return
            if self.stage == "brom":
                # The BootROM acknowledges its EXEC; the loader then
                # runs the downloaded stage.
                self.reply(BSL_REP_ACK, b"")
                self.log("brom-exec-ack")
                self.execute_download()
                return
            # A running FDL jumps without an acknowledgement;
            # spd_dump tolerates whatever the stage announces next.
            self.execute_download()
            return
        if self.stage == "flash":
            self._flash_frame(message_type, payload)
            return
        # t117_fdl answers an unrecognised command value with
        # UNKNOWN_CMD; INVALID_CMD is reserved for known commands
        # misused with a wrong payload length.
        self.reply(BSL_REP_UNKNOWN_CMD, b"")

    def _exec_distance_workaround(self, payload: bytes) -> None:
        """Validate the t117_exec_workaround pattern, then execute.

        The workaround frame's type is the distance value itself; its
        payload is a zero pad (whose length comes from escaped bytes
        of the distance) followed by the entry address repeated as
        252 little-endian words and one align byte when the pad is
        odd. The BROM accepts it once and runs the downloaded stage
        at its load address plus 0x200, past the DHTB header.
        """
        address = self.download.get("address", 0)
        entry = address + dhtb.HEADER_BYTES
        try:
            pad = expected_exec_distance_pad(self.exec_distance, entry)
        except ValueError:
            self.reply(BSL_REP_VERIFY_ERROR, b"")
            return
        expected = (
            bytes(pad)
            + struct.pack("<I", entry) * 252
            + bytes(exec_distance_trailing(pad))
        )
        if payload != expected:
            self.reply(BSL_REP_VERIFY_ERROR, b"")
            return
        self.exec_distance_done = True
        self.log(f"brom-exec-distance {self.exec_distance:#x} entry={entry:#x}")
        self.execute_download()

    def execute_download(self) -> None:
        """Run the downloaded blob and hand control to its stage."""
        address = self.download.get("address", 0)
        blob = bytes(self.download_buffer)
        digest = hashlib.sha256(blob).hexdigest()
        self.log(f"exec {address:#010x} sha256={digest[:12]}")
        if not blob:
            self.log("exec-failed-empty")
            return
        action = self._action_for(blob)
        if action is None:
            self.log("exec-unknown-image")
            return
        action(self)

    # Flash-stage frame handling.

    def _flash_frame(self, message_type: int, payload: bytes) -> None:
        if message_type == BSL_CMD_ENABLE_WRITE_FLASH:
            self.write_enabled = True
            self.reply(BSL_REP_ACK, b"")
            return
        if message_type == BSL_CMD_READ_FLASH:
            if len(payload) not in (8, 12):
                self.reply(BSL_REP_INVALID_CMD, b"")
                return
            address, size = struct.unpack(">II", payload[:8])
            offset = struct.unpack(">I", payload[8:12])[0] if len(payload) == 12 else 0
            if size >= MAX_PKT_PAYLOAD:
                # t117_fdl read_flash: a reply chunk that cannot fit in
                # one MAX_PKT_SIZE packet is rejected up front.
                self.reply(BSL_REP_INVALID_CMD, b"")
                return
            try:
                data = self.regions.read(address, offset, size)
            except ValueError:
                self.reply(BSL_REP_OPERATION_FAILED, b"")
                return
            self.reply(BSL_REP_READ_FLASH, data)
            return
        if message_type == BSL_CMD_ERASE_FLASH:
            if len(payload) != 8:
                self.reply(BSL_REP_INVALID_CMD, b"")
                return
            address, size = struct.unpack(">II", payload)
            if not self.write_enabled:
                self.reply(BSL_REP_VERIFY_ERROR, b"")
                return
            if address not in (REGION_BOOT0, REGION_BOOT1):
                # The stock slots next door are never erased: the arm
                # may not spend itself outside the two boot regions.
                self.reply(BSL_REP_OPERATION_FAILED, b"")
                return
            blocks = (size + self.nand.block_bytes - 1) // self.nand.block_bytes
            try:
                self.regions.erase_region_blocks(address, blocks)
            except ValueError:
                self.reply(BSL_REP_OPERATION_FAILED, b"")
                return
            # The arm survives an erase unchecked; only the program
            # commit of END_DATA consumes it.
            self.log(f"flash-erase {address:#010x} blocks={blocks}")
            self.reply(BSL_REP_ACK, b"")
            return
        self.reply(BSL_REP_UNKNOWN_CMD, b"")

    # Program commit: START_DATA with write enabled, then MIDST data,
    # then END_DATA erases, programs and verifies in one transaction.
    def commit_program(self) -> None:
        address = self.download.get("address", 0)
        size = self.download.get("size", 0)
        if len(self.download_buffer) != size:
            self.reply(BSL_REP_VERIFY_ERROR, b"")
            return
        if not self.write_enabled:
            self.reply(BSL_REP_VERIFY_ERROR, b"")
            return
        try:
            self.regions.program(address, bytes(self.download_buffer))
        except ValueError as error:
            self.log(f"flash-error {error}")
            self.write_enabled = False
            self.reply(BSL_REP_OPERATION_FAILED, b"")
            return
        self.write_enabled = False
        self.log(f"flash-program {address:#010x} size={size}")
        self.reply(BSL_REP_ACK, b"")

    def log(self, message: str) -> None:
        self.chain_log.append(message)

    # Power-on boot chain.

    def power_on(self, *, boot_key_held: bool) -> str:
        """Walk the power-on decision order and report the outcome."""
        self.log("power-on")
        # A power cycle reboots every stage: no download is open and
        # no write arm survives it.
        self.download = {}
        self.download_buffer = bytearray()
        self.write_enabled = False
        if boot_key_held:
            self.stage = "brom"
            self.stage_mode = MODE_CRC16
            self.reader = FrameReader(mode=MODE_CRC16, role="device")
            self.sync_seen = False
            self.version_sent = False
            self.exec_distance_done = False
            self.write_enabled = False
            self.usb_session = True
            self.log("enter-brom-usb")
            return "brom-usb"
        self.usb_session = False
        for address, label in (
            (REGION_BOOT0, "boot0"),
            (REGION_BOOT1, "boot1"),
            (REGION_KERNEL, "kernel"),
        ):
            first, count = self.regions.region_of(address)
            region_bytes = self.regions.read(address, 0, count * self.nand.block_bytes)
            try:
                image = dhtb.Image(region_bytes)
            except ValueError:
                self.log(f"{label}-invalid")
                return f"{label}-invalid"
            if not image.verified:
                self.log(f"{label}-sha-mismatch")
                return f"{label}-sha-mismatch"
            action = self.exec_handlers.get(hashlib.sha256(image.payload).hexdigest())
            if action is None:
                self.log(f"{label}-unhandled")
                return f"{label}-unhandled"
            self.log(f"{label}-ok size={image.size:#x}")
            return action(self)
        return "no-stage"

