# SPDX-License-Identifier: GPL-2.0-only
"""Dependency-free self-test of the persistent-boot implementation.

Every test function is pytest-compatible (plain asserts, no fixtures);
running the file directly executes them all and reports a summary:

    python3 scripts/fplinux_cli/persistent/selftest.py

Coverage, per layer:

* wire format: CRC-16 known answer and frame round-trips through the
  escaping layer, both endpoints' word-sum rules and their agreement,
  the phone's full-frame fold for odd and even payloads, idle-flag
  collapsing around sync bursts;
* DHTB containers: the loaders.py byte layout, little-endian words,
  digest verification, truncated images;
* the T117 exec-distance workaround: pad and align combinations for
  plain and escaped distance bytes;
* the whole device: the download chain in all stages, the write gate
  on flash operations, each flash-stage refusal by name (the reply
  codes the t117_fdl reference uses), the installer's discovery
  gates, and the full stock-to-FPLinux-to-stock end-to-end run.
"""

from __future__ import annotations

import struct
import sys
import traceback

from . import dhtb
from .client import DownloadClient, ProtocolError
from .frames import (
    HDLC_HEADER,
    MODE_CRC16,
    MODE_SUM16,
    FrameReader,
    crc16,
    device_sum16,
    encode_check_baud,
    encode_packet,
    sender_sum16,
    unescape,
    verify_frame,
)
from .install import (
    InstallError,
    Installer,
    build_test_stage_images,
    bind_stage_actions,
    read_region_image,
    stock_phone,
    toolchain_demo,
)
from .nandsim import (
    BSL_CMD_CONNECT,
    BSL_CMD_END_DATA,
    BSL_CMD_ENABLE_WRITE_FLASH,
    BSL_CMD_ERASE_FLASH,
    BSL_CMD_MIDST_DATA,
    BSL_CMD_READ_FLASH,
    BSL_CMD_START_DATA,
    BSL_REP_ACK,
    BSL_REP_DOWN_DEST_ERROR,
    BSL_REP_DOWN_EARLY_END,
    BSL_REP_DOWN_SIZE_ERROR,
    BSL_REP_INVALID_CMD,
    BSL_REP_OPERATION_FAILED,
    BSL_REP_READ_FLASH,
    BSL_REP_UNKNOWN_CMD,
    BSL_REP_VERIFY_ERROR,
    MAX_PKT_PAYLOAD,
    REGION_BOOT0,
    REGION_BOOT1,
    REGION_KERNEL,
    REGION_USER,
    SimulatedDevice,
    exec_distance_trailing,
    expected_exec_distance_pad,
)


def _device_frame(message_type: int, payload: bytes, *, mode: str) -> bytes:
    return encode_packet(message_type, payload, mode=mode, role="device")


def _raw_frame(frame: bytes) -> bytes:
    head = frame.find(bytes([HDLC_HEADER]))
    tail = frame.rfind(bytes([HDLC_HEADER]))
    return unescape(frame[head + 1 : tail])


def test_crc16_known_answer() -> None:
    # CRC-16/CCITT-FALSE variant with zero init, as spd_crc16 drives
    # the BootROM: the reference value of the check string.
    assert crc16(b"123456789") == 0x31C3
    assert crc16(b"") == 0


def test_sum16_endpoint_agreement() -> None:
    """Host-sent frames pass the phone's fold; phone frames pass CHK_ORIG."""
    for length in range(0, 65):
        payload = bytes((index * 7 + 0x7D if index % 3 else 0x7E if index % 5 else index) & 0xFF
            for index in range(length))
        host_frame = encode_packet(0x0002, payload, mode=MODE_SUM16, role="host")
        assert verify_frame(
            _raw_frame(host_frame), mode=MODE_SUM16, role="device"
        ), f"phone rejected host frame {length}"
        phone_frame = _device_frame(0x0080, payload, mode=MODE_SUM16)
        assert verify_frame(
            _raw_frame(phone_frame), mode=MODE_SUM16, role="host"
        ), f"host rejected phone frame {length}"
    # The two sender laws differ exactly on odd covered lengths, which
    # is what the sender's swap-even rule and the phone's always-swap
    # rule add up to on the wire.
    sample = b"\x01\x02\x03"
    assert sender_sum16(sample * 2) == device_sum16(sample * 2)
    assert sender_sum16(sample) != device_sum16(sample)


def test_escape_roundtrip() -> None:
    tricky = bytes([0x7D, 0x7E, 0x5D, 0x5E, 0x00, 0xFF, 0x7D, 0x7E])
    for mode in (MODE_CRC16, MODE_SUM16):
        frame = encode_packet(0x0080, tricky, mode=mode)
        assert frame.count(bytes([HDLC_HEADER])) == 2  # only the delimiters
        reader = FrameReader(mode=mode, role="host")
        reader.feed(frame)
        assert reader.frames() == [(0x80, tricky)]


def test_frame_reader_collapses_idle_flags() -> None:
    """Sync bursts between frames never shadow the frames after them."""
    host = (
        _device_frame(0x0080, b"one", mode=MODE_SUM16)
        + encode_check_baud(4)
        + _device_frame(0x0080, b"two", mode=MODE_SUM16)
        + encode_check_baud(1)
        + _device_frame(0x0080, b"three", mode=MODE_CRC16)
    )
    reader = FrameReader(mode=MODE_SUM16, role="host")
    reader.feed(host[: len(host) // 2])  # split anywhere: replayed state
    reader.feed(host[len(host) // 2 :])
    frames = []
    while True:
        found = reader.frames()
        if not found:
            break
        frames.extend(found)
    payloads = [payload for _type, payload in frames]
    assert payloads == [b"one", b"two"]  # CRC-16 frame fails sum16 checks


def test_dhtb_layout_matches_loaders() -> None:
    payload = b"\x01" * 100
    blob = dhtb.wrap(payload)
    assert blob[:8] == b"DHTB\x01\x00\x00\x00"
    assert int.from_bytes(blob[4:8], "little") == 1
    assert int.from_bytes(blob[0x30:0x34], "little") == len(payload)
    image = dhtb.Image(blob)
    assert image.verified and image.payload == payload and image.image_size() == 0x200 + 100
    corrupt = bytearray(blob)
    corrupt[0x210] ^= 0xFF
    assert not dhtb.Image(bytes(corrupt)).verified
    try:
        dhtb.Image(blob[:-1])
    except ValueError:
        pass
    else:
        raise AssertionError("truncated DHTB payload must raise")


def test_exec_distance_packet_shapes() -> None:
    """Pad shrinks per escaped distance byte; odd pads take one align."""
    cases = {0x0102: 3, 0x0100 + 0x7D: 2, 0x7D00 + 0x02: 2, 0x7D7D: 1}
    for distance, pad in cases.items():
        assert expected_exec_distance_pad(distance, 0x6400) == pad
        assert exec_distance_trailing(pad) == pad % 2
    # The phone accepts the exact pattern and runs the downloaded stage,
    # for plain and escaped distance values alike.
    for distance in (0x0102, 0x7D7D):
        images = build_test_stage_images()
        device = SimulatedDevice(exec_distance=distance)
        bind_stage_actions(device, images)
        device.power_on(boot_key_held=True)
        client = DownloadClient(device.transfer)
        client.verbose = None
        client.vendor_start()
        client.brom_handshake()
        client.download(dhtb.wrap(images.nandboot), 0x6200)
        client.exec_download(0x6200, distance=distance)
        assert "brom-exec-distance" in " ".join(device.chain_log)
        assert "Custom FDL1" in client.stage_handshake()
        # Plain EXEC stays gated off after the workaround ran once.
        assert device.exec_distance_done


def test_brom_plain_exec_is_acknowledged() -> None:
    images = build_test_stage_images()
    device = SimulatedDevice()
    bind_stage_actions(device, images)
    device.power_on(boot_key_held=True)
    client = DownloadClient(device.transfer)
    client.verbose = None
    client.vendor_start()
    assert client.brom_handshake() == "SPRD3"
    assert not client.secure_boot
    client.download(dhtb.wrap(images.nandboot), 0x6200)
    client.exec_download(0x6200)
    client.stage_handshake()
    client.download(dhtb.wrap(images.flash_stage), 0x80100000)
    client.exec_download(0x80100000, expect_ack=False)
    assert "flash" in client.stage_handshake()
    assert "nandboot-download-mode" in " ".join(device.chain_log)


def test_flash_write_gate() -> None:
    """Every program and erase needs ENABLE_WRITE_FLASH first."""
    images = build_test_stage_images()
    device = stock_phone(images)
    device.power_on(boot_key_held=True)
    client = DownloadClient(device.transfer)
    client.verbose = None
    client.vendor_start()
    client.brom_handshake()
    client.download(dhtb.wrap(images.nandboot), 0x6200)
    client.exec_download(0x6200)
    client.stage_handshake()
    client.download(dhtb.wrap(images.flash_stage), 0x80100000)
    client.exec_download(0x80100000, expect_ack=False)
    client.stage_handshake()
    # Erase before the gate: rejected.
    try:
        client.erase_flash(REGION_BOOT0, 0x20000)
    except ProtocolError:
        pass
    else:
        raise AssertionError("erase without ENABLE_WRITE_FLASH was accepted")
    # Program before the gate: rejected at START_DATA.
    try:
        client._command(
            BSL_CMD_START_DATA, struct.pack(">II", REGION_BOOT0, 16)
        )
    except ProtocolError:
        pass
    else:
        raise AssertionError("program without ENABLE_WRITE_FLASH was accepted")


def test_flash_reply_codes() -> None:
    """The flash stage's refusals answer with the t117_fdl codes."""
    images = build_test_stage_images()
    device = stock_phone(images)
    device.power_on(boot_key_held=True)
    client = DownloadClient(device.transfer)
    client.verbose = None
    client.vendor_start()
    client.brom_handshake()
    client.download(dhtb.wrap(images.nandboot), 0x6200)
    client.exec_download(0x6200)
    client.stage_handshake()
    client.download(dhtb.wrap(images.flash_stage), 0x80100000)
    client.exec_download(0x80100000, expect_ack=False)
    client.stage_handshake()

    def rejected(command: int, payload: bytes, code: int) -> None:
        try:
            client._command(command, payload)
        except ProtocolError as error:
            assert f"0x{code:02x}" in str(error), str(error)
        else:
            raise AssertionError(f"command 0x{command:x} was accepted")

    # t117_fdl answers an unrecognised command value with UNKNOWN_CMD.
    rejected(0x6566, b"", BSL_REP_UNKNOWN_CMD)
    # Known commands with a wrong payload length answer INVALID_CMD.
    rejected(BSL_CMD_START_DATA, b"\x00" * 4, BSL_REP_INVALID_CMD)
    rejected(BSL_CMD_ERASE_FLASH, b"\x00" * 4, BSL_REP_INVALID_CMD)
    # The write gate sits in front of every program and erase.
    rejected(
        BSL_CMD_START_DATA, struct.pack(">II", REGION_BOOT0, 16), BSL_REP_VERIFY_ERROR
    )
    rejected(
        BSL_CMD_ERASE_FLASH, struct.pack(">II", REGION_BOOT0, 16), BSL_REP_VERIFY_ERROR
    )
    client.enable_write()
    # The read-only slots never stage a download and never erase.
    rejected(
        BSL_CMD_START_DATA,
        struct.pack(">II", REGION_KERNEL, 16),
        BSL_REP_DOWN_DEST_ERROR,
    )
    rejected(
        BSL_CMD_ERASE_FLASH,
        struct.pack(">II", REGION_KERNEL, 16),
        BSL_REP_OPERATION_FAILED,
    )
    # A MIDST chunk beyond the declared size answers DOWN_SIZE_ERROR.
    client._command(BSL_CMD_START_DATA, struct.pack(">II", REGION_BOOT0, 16))
    rejected(BSL_CMD_MIDST_DATA, b"x" * 17, BSL_REP_DOWN_SIZE_ERROR)
    # END before the download is complete answers DOWN_EARLY_END.
    rejected(BSL_CMD_END_DATA, b"", BSL_REP_DOWN_EARLY_END)
    # The arm survives refusals and covers an erase untouched.
    client._command(
        BSL_CMD_ERASE_FLASH, struct.pack(">II", REGION_BOOT0, 0x20000)
    )
    # A completed commit consumes the arm.
    client._command(BSL_CMD_START_DATA, struct.pack(">II", REGION_BOOT0, 4))
    client._command(BSL_CMD_MIDST_DATA, b"fast")
    client._command(BSL_CMD_END_DATA)
    rejected(
        BSL_CMD_ERASE_FLASH,
        struct.pack(">II", REGION_BOOT0, 0x20000),
        BSL_REP_VERIFY_ERROR,
    )


class _SketchFlashStage:
    """A direct transliteration of ``flash_stage_sketch.c``.

    Every handler decision, in the same order, from the C command
    layer: the region table, the write gate, the staging rules, the
    arm's consumption at the program commit, and the reply codes of
    every refusal. It is the executable oracle for
    ``test_c_sketch_reply_parity``: the sketch and the simulator must
    answer the same command script, reply for reply.
    """

    BLOCK_BYTES = 2048 * 64

    def __init__(self) -> None:
        self.addr = REGION_BOOT0
        self.size = 0
        self.received = 0
        self.active = False
        self.armed = False

    @staticmethod
    def region_blocks(region: int) -> int:
        # region_blocks(): production would read the VBM_BOOT table.
        return {
            REGION_BOOT0: 1,
            REGION_BOOT1: 4,
            REGION_KERNEL: 32,
            REGION_USER: 256 - 37,
        }.get(region, 0)

    @classmethod
    def boot_region(cls, region: int) -> bool:
        blocks = cls.region_blocks(region)
        return blocks != 0 and region not in (REGION_KERNEL, REGION_USER)

    def handle(self, command: int, payload: bytes) -> int:
        """Mirror of the flash_stage_main() switch, same order."""
        if command == BSL_CMD_CONNECT:
            return BSL_REP_ACK
        if command == BSL_CMD_ENABLE_WRITE_FLASH:
            self.armed = True
            return BSL_REP_ACK
        if command == BSL_CMD_START_DATA:
            if len(payload) != 8:
                return BSL_REP_INVALID_CMD
            if not self.armed:
                return BSL_REP_VERIFY_ERROR
            address, size = struct.unpack(">II", payload)
            if not self.boot_region(address):
                return BSL_REP_DOWN_DEST_ERROR
            if size > 4 * self.BLOCK_BYTES:
                return BSL_REP_DOWN_SIZE_ERROR
            self.addr = address
            self.size = size
            self.received = 0
            self.active = True
            return BSL_REP_ACK
        if command == BSL_CMD_MIDST_DATA:
            if not self.active or len(payload) > self.size - self.received:
                return BSL_REP_DOWN_SIZE_ERROR
            self.received += len(payload)
            return BSL_REP_ACK
        if command == BSL_CMD_END_DATA:
            if not self.active or self.received != self.size:
                return BSL_REP_DOWN_EARLY_END
            if not self.armed:
                return BSL_REP_VERIFY_ERROR
            self.armed = False
            image_ok = self.boot_region(self.addr) and self.size <= (
                self.region_blocks(self.addr) * self.BLOCK_BYTES
            )
            return BSL_REP_ACK if image_ok else BSL_REP_OPERATION_FAILED
        if command == BSL_CMD_READ_FLASH:
            if len(payload) not in (8, 12):
                return BSL_REP_INVALID_CMD
            address, size = struct.unpack(">II", payload[:8])
            offset = struct.unpack(">I", payload[8:12])[0] if len(payload) == 12 else 0
            if size >= MAX_PKT_PAYLOAD:
                return BSL_REP_INVALID_CMD
            if self.region_blocks(address) == 0:
                return BSL_REP_OPERATION_FAILED
            region_bytes = self.region_blocks(address) * self.BLOCK_BYTES
            if offset >= region_bytes or size > region_bytes - offset:
                return BSL_REP_OPERATION_FAILED
            return BSL_REP_READ_FLASH
        if command == BSL_CMD_ERASE_FLASH:
            if len(payload) != 8:
                return BSL_REP_INVALID_CMD
            if not self.armed:
                return BSL_REP_VERIFY_ERROR
            address, size = struct.unpack(">II", payload)
            if not self.boot_region(address):
                return BSL_REP_OPERATION_FAILED
            blocks = (size + self.BLOCK_BYTES - 1) // self.BLOCK_BYTES
            if blocks > self.region_blocks(address):
                return BSL_REP_OPERATION_FAILED
            return BSL_REP_ACK
        return BSL_REP_UNKNOWN_CMD


def test_c_sketch_reply_parity() -> None:
    """The C sketch and the simulator answer a script, reply for reply.

    Every refusal path of ``flash_stage_sketch.c`` is scripted below
    once and answered by both implementations: reply codes must match
    at each step. Images larger than the sketch's boot-sized stage
    buffer differ by design (the sketch refuses them at START_DATA,
    the model defers to the commit) and are kept out of the script.
    """
    images = build_test_stage_images()
    device = stock_phone(images)
    device.power_on(boot_key_held=True)
    client = DownloadClient(device.transfer)
    client.verbose = None
    client.vendor_start()
    client.brom_handshake()
    client.download(dhtb.wrap(images.nandboot), 0x6200)
    client.exec_download(0x6200)
    client.stage_handshake()
    client.download(dhtb.wrap(images.flash_stage), 0x80100000)
    client.exec_download(0x80100000, expect_ack=False)
    client.stage_handshake()

    def sim_reply(command: int, payload: bytes) -> int:
        try:
            client._command(command, payload)
        except ProtocolError as error:
            code = str(error).rsplit("with 0x", 1)
            assert len(code) == 2, str(error)
            return int(code[1], 16)
        return BSL_REP_ACK

    sketch = _SketchFlashStage()
    boot0, kernel = REGION_BOOT0, REGION_KERNEL
    script = [
        ("unknown command value", 0x6566, b""),
        ("START with a wrong length", BSL_CMD_START_DATA, b"\x00" * 4),
        ("ERASE with a wrong length", BSL_CMD_ERASE_FLASH, b"\x00" * 4),
        ("READ with a wrong length", BSL_CMD_READ_FLASH, b"\x00" * 9),
        ("READ chunk over one packet", BSL_CMD_READ_FLASH,
         struct.pack(">III", boot0, MAX_PKT_PAYLOAD, 0)),
        ("READ of an unknown region", BSL_CMD_READ_FLASH,
         struct.pack(">III", 0x12345678, 8, 0)),
        ("READ beyond the boot region", BSL_CMD_READ_FLASH,
         struct.pack(">III", boot0, 8, 0x21000)),
        ("plain READ", BSL_CMD_READ_FLASH, struct.pack(">III", boot0, 8, 0)),
        ("program before the gate", BSL_CMD_START_DATA,
         struct.pack(">II", boot0, 1)),
        ("MIDST before any START", BSL_CMD_MIDST_DATA, b"x"),
        ("END before any START", BSL_CMD_END_DATA, b""),
        ("erase before the gate", BSL_CMD_ERASE_FLASH,
         struct.pack(">II", boot0, 0x20000)),
        ("arm the write gate", BSL_CMD_ENABLE_WRITE_FLASH, b""),
        ("program aimed at the kernel", BSL_CMD_START_DATA,
         struct.pack(">II", kernel, 16)),
        ("erase aimed at the kernel", BSL_CMD_ERASE_FLASH,
         struct.pack(">II", kernel, 16)),
        ("erase beyond the boot0 slot", BSL_CMD_ERASE_FLASH,
         struct.pack(">II", boot0, 0x40000)),
        ("erase of boot0, armed", BSL_CMD_ERASE_FLASH,
         struct.pack(">II", boot0, 0x20000)),
        ("program after an erase kept the arm", BSL_CMD_START_DATA,
         struct.pack(">II", boot0, 4096)),
        ("MIDST chunk one", BSL_CMD_MIDST_DATA, b"x" * 512),
        ("MIDST chunk beyond the size", BSL_CMD_MIDST_DATA, b"y" * 4096),
        ("END before the download completes", BSL_CMD_END_DATA, b""),
        ("MIDST chunk two completes the image", BSL_CMD_MIDST_DATA, b"y" * 3584),
        ("END commits the program", BSL_CMD_END_DATA, b""),
        ("erase after the commit consumed the arm", BSL_CMD_ERASE_FLASH,
         struct.pack(">II", boot0, 0x20000)),
    ]
    for description, command, payload in script:
        expected = sketch.handle(command, payload)
        actual = sim_reply(command, payload)
        assert actual == expected, (
            f"{description}: sketch replied 0x{expected:02x}, "
            f"simulator replied 0x{actual:02x}"
        )


def test_discovery_gates() -> None:
    images = build_test_stage_images()

    # Unknown layout in boot0: refused.
    device = stock_phone(images)
    device.regions.program(REGION_BOOT0, b"NOT-A-DHTB-IMAGE" * 32)
    device.power_on(boot_key_held=True)
    installer = Installer(device.transfer, images)
    try:
        installer.discover()
    except InstallError as error:
        assert "refusing" in str(error)
    else:
        raise AssertionError("discovery accepted an unknown boot0 layout")

    # Signed stock chain: the signature gate stops the install.
    device = stock_phone(images)
    stock0, _stock1, _kernel = dhtb.wrap(b"STOCK-S0"), dhtb.wrap(b"STOCK-S1"), dhtb.wrap(b"K")
    signature_header = bytearray(0x60)
    struct.pack_into("<I", signature_header, 0x10, len(stock0) - 0x200)
    struct.pack_into("<I", signature_header, 0x18, 0x200)
    struct.pack_into("<I", signature_header, 0x20, 0x34)
    struct.pack_into("<I", signature_header, 0x28, len(stock0) + 0x60)
    signed = stock0 + bytes(signature_header) + b"\x11" * 0x34
    device.regions.program(REGION_BOOT0, signed)
    device.power_on(boot_key_held=True)
    installer = Installer(device.transfer, images)
    try:
        backup = installer.discover()
        assert backup.boot0.contains_signature
        # discover() must raise, so if we get here the gate is missing.
        raise AssertionError("discovery accepted a signed boot chain")
    except InstallError as error:
        assert "signature" in str(error)

    # Reinstall over the FPLinux stage: refused with a clear message.
    device = stock_phone(images)
    device.power_on(boot_key_held=True)
    installer = Installer(device.transfer, images)
    backup = installer.discover()
    installer.install(backup)
    device.power_on(boot_key_held=True)
    reinstall = Installer(device.transfer, images)
    try:
        reinstall.discover()
    except InstallError as error:
        assert "already" in str(error)
    else:
        raise AssertionError("discovery accepted a reinstall over the nandboot stage")


def test_all_stage_images_boot_their_actions() -> None:
    """The registry resolves every stage by payload digest, both paths."""
    images = build_test_stage_images()
    device = stock_phone(images)
    device.power_on(boot_key_held=False)
    assert device.power_on(boot_key_held=False) == "stock-rtos"


def test_persistent_boot_end_to_end() -> None:
    result = toolchain_demo()
    assert result["stock_boot"] == "stock-rtos"
    assert result["download_mode"] == "brom-usb"
    assert result["discovered"]["chip_id"] == "0x98180000"
    assert result["install"] and result["verify"]
    assert result["fplinux_boot"] == "linux-session"
    assert result["tampered_boot"] == "boot1-sha-mismatch"
    assert result["restored_boot"] == "stock-rtos"
    log = " ".join(result["chain_log"])
    assert "nandboot-download-mode" in log
    assert "boot1-sha-mismatch" in log
    assert "linux-session" in log


def test_persistent_boot_end_to_end_with_distance() -> None:
    for distance in (0x0102, 0x7D7D):
        result = toolchain_demo(exec_distance=distance)
        assert result["install"] and result["verify"]
        assert result["fplinux_boot"] == "linux-session"
        assert result["restored_boot"] == "stock-rtos"


def test_backup_receipt_covers_both_regions() -> None:
    images = build_test_stage_images()
    device = stock_phone(images)
    device.power_on(boot_key_held=True)
    installer = Installer(device.transfer, images)
    backup = installer.discover()
    assert backup.boot0.region == REGION_BOOT0
    assert backup.boot1.region == REGION_BOOT1
    assert backup.boot0.image.startswith(dhtb.MAGIC)
    assert backup.boot1.payload_sha256
    # The saved bytes are exactly what the phone's stock regions held.
    restored = read_region_image(installer.client, REGION_BOOT0)
    assert restored.image == backup.boot0.image


def _run_all() -> int:
    tests = sorted(
        (name, callback)
        for name, callback in globals().items()
        if name.startswith("test_") and callable(callback)
    )
    failures = 0
    for name, callback in tests:
        try:
            callback()
        except Exception:  # noqa: BLE001 -- report and continue
            failures += 1
            print(f"FAIL {name}")
            traceback.print_exc()
        else:
            print(f"ok   {name}")
    print(f"\n{len(tests) - failures}/{len(tests)} tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(_run_all())

