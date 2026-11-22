# SPDX-License-Identifier: GPL-2.0-only
"""Write small class files whose stack maps the verifier must judge.

usage: make-verify-classes.py OUTPUT_DIR

Writes VerifyLong, VerifyNoMap, VerifyUnordered and VerifyDuplicate into
OUTPUT_DIR (the directory of package fplinux.fixtures.jit). Each class extends
VerifyBody and implements int f(int) with the same 618 byte method:

    0   iload_1; ifne 609
    4   600 nops
    604 iconst_1; ireturn                  f(0) == 1
    606 nop; iconst_0; ireturn             dead code after the return
    609 iload_1; iconst_1; if_icmpne 616
    614 iconst_2; ireturn                  f(1) == 2
    616 iconst_3; ireturn                  f(other) == 3

Stack maps are needed at the branch targets 609 and 616, and at 606, which
follows an unconditional transfer without being a branch target. Every offset
is past the 512 bytes the verifier keeps in its stack map cache. The classes
differ only in their stack maps:

    VerifyLong       maps at 606, 609 and 616 in order: accepted
    VerifyNoMap      no map at 606: rejected, the instruction after the
                     return has no map
    VerifyUnordered  maps in the order 609, 606, 616: rejected
    VerifyDuplicate  two maps at 606: rejected
"""

import os
import struct
import sys

PACKAGE = "fplinux/fixtures/jit/"


def utf8(text):
    data = text.encode("ascii")
    return b"\x01" + struct.pack(">H", len(data)) + data


def build(name, map_offsets):
    # Constant pool, 1-based: the numbers below are used in the code.
    pool = [
        utf8(PACKAGE + name),  # 1
        b"\x07" + struct.pack(">H", 1),  # 2 this class
        utf8(PACKAGE + "VerifyBody"),  # 3
        b"\x07" + struct.pack(">H", 3),  # 4 super class
        utf8("<init>"),  # 5
        utf8("()V"),  # 6
        b"\x0c" + struct.pack(">HH", 5, 6),  # 7
        b"\x0a" + struct.pack(">HH", 4, 7),  # 8 VerifyBody.<init>
        utf8("Code"),  # 9
        utf8("f"),  # 10
        utf8("(I)I"),  # 11
        utf8("StackMap"),  # 12
    ]

    nop, iconst_0, iconst_1, iconst_2, iconst_3 = 0x00, 0x03, 0x04, 0x05, 0x06
    iload_1, ireturn, ifne, if_icmpne = 0x1B, 0xAC, 0x9A, 0xA0
    code = bytearray([iload_1, ifne]) + struct.pack(">h", 609 - 1)
    code += bytes([nop]) * 600
    code += bytes([iconst_1, ireturn])  # 604
    code += bytes([nop, iconst_0, ireturn])  # 606
    code += bytes([iload_1, iconst_1, if_icmpne])  # 609
    code += struct.pack(">h", 616 - 611)
    code += bytes([iconst_2, ireturn])  # 614
    code += bytes([iconst_3, ireturn])  # 616
    assert len(code) == 618

    # Locals: this (the class itself) and the int argument; empty stack.
    frame = struct.pack(">H", 2) + b"\x07" + struct.pack(">H", 2) + b"\x01" + struct.pack(">H", 0)
    maps = struct.pack(">H", len(map_offsets))
    for offset in map_offsets:
        maps += struct.pack(">H", offset) + frame

    def attribute(name_index, info):
        return struct.pack(">HI", name_index, len(info)) + info

    f_code = (
        struct.pack(">HHI", 2, 2, len(code))
        + code
        + struct.pack(">H", 0)
        + struct.pack(">H", 1)
        + attribute(12, maps)
    )
    init_code = (
        struct.pack(">HHI", 1, 1, 5) + bytes([0x2A, 0xB7, 0, 8, 0xB1]) + struct.pack(">HH", 0, 0)
    )

    out = struct.pack(">IHHH", 0xCAFEBABE, 3, 45, len(pool) + 1) + b"".join(pool)
    out += struct.pack(
        ">HHHHH", 0x0021, 2, 4, 0, 0
    )  # access, this, super, no interfaces, no fields
    out += struct.pack(">H", 2)  # methods
    out += struct.pack(">HHHH", 0x0001, 5, 6, 1) + attribute(9, init_code)
    out += struct.pack(">HHHH", 0x0001, 10, 11, 1) + attribute(9, f_code)
    out += struct.pack(">H", 0)  # class attributes
    return out


CLASSES = {
    "VerifyLong": [606, 609, 616],
    "VerifyNoMap": [609, 616],
    "VerifyUnordered": [609, 606, 616],
    "VerifyDuplicate": [606, 606, 609, 616],
}


def main():
    directory = sys.argv[1]
    for name, offsets in CLASSES.items():
        with open(os.path.join(directory, name + ".class"), "wb") as target:
            target.write(build(name, offsets))


main()
