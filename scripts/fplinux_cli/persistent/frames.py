# SPDX-License-Identifier: GPL-2.0-only
"""Unisoc download wire format: HDLC framing and the two checksum modes.

The byte layout, escaping, checksums and the byte-swap rule are ports of
Spreadtrum's host tool (spd_dump.c) and the UMS9117 FDL counterpart
(t117_fdl/packet.c). Frame layout on the wire:

    0x7e | type BE16 | length BE16 | payload | checksum BE16 | 0x7e

The checksum mode differs per boot phase:

* BootROM phase: CRC-16/CCITT (poly 0x11021, init 0, MSB first, no
  reflection) over ``type BE16 | length BE16 | payload``.
* FDL phase: the Spreadtrum 16-bit one's-complement word sum over the
  same bytes. The transmitted checksum is byte-swapped when the
  covered length is even, matching ``spd_checksum(..., CHK_FIXZERO)``
  on the sender and ``... CHK_ORIG`` on the receiver.

CHECK_BAUD is not a frame: the host sends a burst of raw ``0x7e`` sync
bytes and the device answers BSL_REP_VER.
"""

from __future__ import annotations

HDLC_HEADER = 0x7E
HDLC_ESCAPE = 0x7D

# spd_dump.c: FLAGS_CRC16.
MODE_CRC16 = "crc16"
MODE_SUM16 = "sum16"


def crc16(data: bytes, crc: int = 0) -> int:
    """Return the BootROM-phase CRC-16 as spd_crc16 computes it."""
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            carry = crc >> 15
            crc = ((crc << 1) ^ (0x11021 * (carry & 1))) & 0x1FFFF
        # The xor term carries the same bit 16 as the shifted register,
        # so bit 16 always cancels and the register stays 16 bit, as in
        # the unsigned C arithmetic of spd_crc16.
        crc &= 0xFFFF
    return crc & 0xFFFF


def _word_sum(data: bytes, count: int | None = None) -> int:
    """Sum 16-bit little-endian words plus a raw trailing byte.

    Both endpoints accumulate bytes pairwise into the same numeric
    sum: the host reads ``s[1] << 8 | s[0]`` in spd_checksum, the phone
    walks a ``uint16_t *`` in dl_fastchk16; an odd trailing byte is
    added unshifted by both. The checksum bytes on the wire therefore
    land pairwise with payload bytes of either parity, exactly as the
    phone's full-frame sum does.
    """
    crc = 0
    size = len(data) if count is None else count
    index = 0
    while size - index > 1:
        crc += data[index] | (data[index + 1] << 8)
        index += 2
    if size - index == 1:
        crc += data[index]
    return crc


def _fold(crc: int) -> int:
    crc &= 0xFFFF
    crc = (crc >> 16) + crc
    crc += crc >> 16
    return crc & 0xFFFF


def _byteswap16(value: int) -> int:
    return (value >> 8) | ((value & 0xFF) << 8)


def sender_sum16(covered: bytes) -> int:
    """Checksum the host writes behind type|length|payload.

    Port of spd_checksum(..., CHK_FIXZERO): the folded complement is
    byte-swapped exactly when the covered length is even (the
    remainder after the pair loop is zero). The phone's full-frame
    fold accepts both parities with this rule, which is what makes
    odd-length downloads legal on the wire.
    """
    checksum = _fold(_word_sum(covered))
    checksum = ~checksum & 0xFFFF
    if len(covered) % 2 == 0:
        checksum = _byteswap16(checksum)
    return checksum


def device_sum16(covered: bytes) -> int:
    """Checksum the phone writes behind type|length|payload.

    Port of dl_escapedata: fastchk16 folded complement then an
    unconditional byte swap, both parities alike.
    """
    checksum = _fold(_word_sum(covered))
    checksum = ~checksum & 0xFFFF
    return _byteswap16(checksum)


def receiver_sum16(covered: bytes) -> int:
    """Checksum a host recomputes over type|length|payload.

    Port of spd_checksum(..., CHK_ORIG): the remainder after the
    pair loop (0 or 1) is always below CHK_ORIG, so the expected
    value is the swapped folded complement regardless of parity,
    which is exactly what the phone always sends.
    """
    checksum = _fold(_word_sum(covered))
    checksum = ~checksum & 0xFFFF
    return _byteswap16(checksum)


def frame_valid_sum16(raw: bytes) -> bool:
    """Return whether a whole frame passes the phone's acceptance rule.

    Port of dl_fastchk16 over the unescaped type|length|payload|
    checksum: the folded one's-complement of the word sum must be
    zero, i.e. the sum itself folds to 0xFFFF. Odd-length frames sum
    the trailing byte unshifted, exactly like the ``len & 1`` branch;
    this is the rule that makes the sender's swap-even/keep-odd
    checksum valid on the wire for both parities.
    """
    if len(raw) < 6:
        return False
    return _fold(_word_sum(raw)) == 0xFFFF


def escape(data: bytes) -> bytes:
    """Return data with 0x7d/0x7e escaped, as dl_escapedata does."""
    out = bytearray()
    for byte in data:
        if byte in (HDLC_HEADER, HDLC_ESCAPE):
            out.append(HDLC_ESCAPE)
            out.append(byte ^ 0x20)
        else:
            out.append(byte)
    return bytes(out)


def unescape(data: bytes) -> bytes:
    """Reverse escape(); an escape not followed by a valid byte stops."""
    out = bytearray()
    pending = 0
    for byte in data:
        if pending:
            if byte in (HDLC_HEADER ^ 0x20, HDLC_ESCAPE ^ 0x20):
                out.append(byte ^ 0x20)
            else:
                return bytes(out)  # the phone aborts the frame here
            pending = 0
        elif byte == HDLC_ESCAPE:
            pending = 0x20
        else:
            out.append(byte)
    return bytes(out)


def encode_packet(
    message_type: int, payload: bytes, *, mode: str, role: str = "host"
) -> bytes:
    """Return one escaped 0x7e-delimited frame for one checksum mode.

    ``role`` picks the endpoint's checksum law: the host sends
    CHK_FIXZERO (swap when the covered length is even), the phone
    always swaps (dl_escapedata). In CRC-16 mode both roles emit the
    same CRC.
    """
    if len(payload) > 0xFFFF:
        raise ValueError("message too long")
    raw = bytearray()
    raw += message_type.to_bytes(2, "big")
    raw += len(payload).to_bytes(2, "big")
    raw += payload
    if mode == MODE_CRC16:
        check = crc16(raw)
    elif mode == MODE_SUM16:
        check = device_sum16(raw) if role == "device" else sender_sum16(raw)
    else:
        raise ValueError(f"unknown checksum mode: {mode}")
    raw += check.to_bytes(2, "big")
    frame = bytearray([HDLC_HEADER])
    frame += escape(raw)
    frame.append(HDLC_HEADER)
    return bytes(frame)


def encode_check_baud(byte_count: int) -> bytes:
    """Return the raw 0x7e sync burst; CHECK_BAUD is not a frame."""
    if byte_count <= 0:
        raise ValueError("check-baud burst needs at least one byte")
    return bytes([HDLC_HEADER]) * byte_count


def verify_frame(raw: bytes, *, mode: str, role: str = "host") -> bool:
    """Return whether one unescaped frame carries a valid checksum.

    ``role`` selects the checksum law of the endpoint, as the two
    stacks differ: the host recomputes the sender form over
    type|length|payload and compares (spd_dump's CHK_ORIG, which is
    why the phone's always-swap checksum matches it), while the phone
    folds the whole frame including the checksum and accepts at zero
    (dl_fastchk16). In CRC-16 mode both roles compute the same CRC.
    """
    if len(raw) < 6:
        return False
    if mode == MODE_CRC16:
        body, check = raw[:-2], int.from_bytes(raw[-2:], "big")
        return crc16(body) == check
    if mode == MODE_SUM16:
        if role == "device":
            return frame_valid_sum16(raw)
        body, check = raw[:-2], int.from_bytes(raw[-2:], "big")
        return receiver_sum16(body) == check
    raise ValueError(f"unknown checksum mode: {mode}")


class FrameReader:
    """Collect frames from a byte stream the way dl_get_packet does.

    The reader consumes complete 0x7e-delimited frames and ignores
    anything before the first opening flag, with the FDL's own
    tolerance: runs of idle flags collapse, and a frame whose payload
    is shorter than its length prefix or whose checksum fails is
    discarded for the caller's retry logic to reconstruct. ``role``
    passes the endpoint's checksum law through to :func:`verify_frame`
    ("host" recomputes the sender form, "device" folds the whole
    frame like the FDL does).
    """

    def __init__(self, *, mode: str, role: str = "host") -> None:
        self.mode = mode
        self.role = role
        self._buffer = bytearray()

    def feed(self, chunk: bytes) -> None:
        self._buffer += chunk

    def _next_frame(self) -> tuple[int, bytes] | None:
        buf = self._buffer
        if len(buf) < 6:
            return None
        # HDLC idle handling: a run of consecutive 0x7e flags collapses
        # to one delimiter, so sync bursts and aborted frames before a
        # real frame never shadow the frame that follows them.
        first = 0
        while (
            first + 1 < len(buf)
            and buf[first] == HDLC_HEADER
            and buf[first + 1] == HDLC_HEADER
        ):
            first += 1
        if first:
            del buf[:first]
        start = buf.find(bytes([HDLC_HEADER]))
        if start < 0:
            del buf[:]
            return None
        if start:
            del buf[:start]
        end = buf.find(bytes([HDLC_HEADER]), 1)
        if end < 0:
            if len(buf) > 2048 + 16:
                del buf[:]
            return None
        raw = unescape(bytes(buf[1:end]))
        declared = int.from_bytes(raw[2:4], "big") if len(raw) >= 4 else -1
        if (
            len(raw) < 6
            or declared != len(raw) - 6
            or not verify_frame(raw, mode=self.mode, role=self.role)
        ):
            del buf[: end + 1]
            return None
        del buf[: end + 1]
        return int.from_bytes(raw[0:2], "big"), raw[4:-2]

    def frames(self) -> list[tuple[int, bytes]]:
        """Return and clear all complete, valid frames in the buffer."""
        found: list[tuple[int, bytes]] = []
        while True:
            frame = self._next_frame()
            if frame is None:
                break
            found.append(frame)
        return found

