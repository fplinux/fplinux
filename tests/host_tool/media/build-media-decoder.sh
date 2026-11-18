#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

if [ "$#" -ne 1 ]; then
	echo "usage: $0 BUILD-DIRECTORY" >&2
	exit 2
fi

directory=$1
mkdir -p -- "$directory/source"
tar -xjf /opt/quality/src/mpg123.tar.bz2 --strip-components=1 -C "$directory/source"
cd -- "$directory/source"
./configure --disable-shared --enable-static --disable-components \
	--enable-libmpg123 --disable-network --with-network=none \
	--with-audio=dummy --with-cpu=generic --disable-layer1 --disable-layer2 \
	--disable-8bit --disable-32bit --disable-real --disable-icy \
	--disable-equalizer --disable-moreinfo --disable-buffer
make
