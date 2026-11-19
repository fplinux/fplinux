#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

exec ./configure \
	--prefix=/usr --incdir=/usr/include/fplinux-amr \
	--build-suffix=-fplinux-amr --enable-shared --disable-static \
	--enable-small --disable-faan --disable-autodetect --disable-everything \
	--disable-programs --disable-avformat --disable-avdevice \
	--disable-avfilter --disable-swscale --disable-swresample \
	--disable-network --disable-iconv --disable-doc --disable-debug \
	--enable-decoder=amrnb,adpcm_ima_wav "$@"
