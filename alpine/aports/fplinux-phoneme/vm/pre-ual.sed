# SPDX-License-Identifier: GPL-2.0-only
# Convert the pre-UAL ("divided") ARM syntax emitted by CLDC-HI loopgen into UAL for LLVM's
# integrated assembler (GNU as accepts both; clang -c rejects pre-UAL).
s/^\([[:space:]]*\)\(ldr\|str\)\(eq\|ne\|cs\|hs\|cc\|lo\|mi\|pl\|vs\|vc\|hi\|ls\|ge\|lt\|gt\|le\|al\)\(sb\|sh\|b\|h\|d\)\([[:space:]]\)/\1\2\4\3\5/
s/^\([[:space:]]*\)\(ldm\|stm\)\(eq\|ne\|cs\|hs\|cc\|lo\|mi\|pl\|vs\|vc\|hi\|ls\|ge\|lt\|gt\|le\|al\)\(ia\|ib\|da\|db\|fd\|fa\|ed\|ea\)\([[:space:]]\)/\1\2\4\3\5/
s/^\([[:space:]]*\)\(add\|sub\|rsb\|adc\|sbc\|rsc\|and\|orr\|eor\|bic\|mov\|mvn\|mul\|mla\|lsl\|lsr\|asr\|ror\)\(eq\|ne\|cs\|hs\|cc\|lo\|mi\|pl\|vs\|vc\|hi\|ls\|ge\|lt\|gt\|le\|al\)s\([[:space:]]\)/\1\2s\3\4/
s/^\([[:space:]]*\)fstmfd[sd]\([[:space:]]\)/\1vstmdb\2/
s/^\([[:space:]]*\)fldmfd[sd]\([[:space:]]\)/\1vldmia\2/
s/^\([[:space:]]*\)fstmia[sd]\([[:space:]]\)/\1vstmia\2/
s/^\([[:space:]]*\)fldmia[sd]\([[:space:]]\)/\1vldmia\2/
s/^\([[:space:]]*\)fmsrr\([a-z]*\)\([[:space:]]*\){\(s[0-9]*\), *\(s[0-9]*\)}, *\(r[0-9]*\), *\(r[0-9]*\)/\1vmov\2\3\4, \5, \6, \7/
s/^\([[:space:]]*\)fmrrs\([a-z]*\)\([[:space:]]*\)\(r[0-9]*\), *\(r[0-9]*\), *{\(s[0-9]*\), *\(s[0-9]*\)}/\1vmov\2\3\4, \5, \6, \7/
