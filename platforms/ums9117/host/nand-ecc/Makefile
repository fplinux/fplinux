# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g0 -Wall -Wextra -fno-ident
LIBS ?= -static

all: ums9117-nand-ecc

ums9117-nand-ecc: ums9117-nand-ecc.o bch.o fplinux-cli.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

ums9117-nand-ecc.o: ums9117-nand-ecc.c bch.h fplinux-cli.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -std=c11 -c -o $@ $<

bch.o: bch.c bch.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c -o $@ $<

fplinux-cli.o: fplinux-cli.c fplinux-cli.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Werror -std=c11 -c -o $@ $<

clean:
	rm -f ums9117-nand-ecc *.o

.PHONY: all clean
