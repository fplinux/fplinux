#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Build-time self-test of the ARM VM under qemu-arm: the CLDC vectors on the
# bare VM and as a MIDlet, and the headless hello MIDlet. The vectors' oracle
# is OpenJDK 8 HotSpot (vectors-hotspot.expected); vectors-cldc.diff lists the
# known CLDC-HI differences (tan rounding, "0.0010", NaN/Infinity parsing).
#
# usage: run.sh WORK_DIR
# environment: FPLINUX_PM_JDK, FPLINUX_PM_PREVERIFY, FPLINUX_PM_QEMU_ARM,
#              FPLINUX_PM_CLASSES (system classes.zip), FPLINUX_PM_CLDC_VM,
#              FPLINUX_PM_HEADLESS, FPLINUX_PM_SYSROOT (target libraries)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
work=${1:?usage: run.sh WORK_DIR}
jdk=${FPLINUX_PM_JDK:?}
preverify=${FPLINUX_PM_PREVERIFY:?}
qemu_arm=${FPLINUX_PM_QEMU_ARM:?}
classes=${FPLINUX_PM_CLASSES:?}
cldc_vm=${FPLINUX_PM_CLDC_VM:?}
headless=${FPLINUX_PM_HEADLESS:?}
sysroot=${FPLINUX_PM_SYSROOT:?}

# Compiles one fixture package against the system classes into a JAR.
build_jar() {
	name=$1
	manifest=$2
	mkdir -p "${work}/${name}/classes" "${work}/${name}/preverified/META-INF"
	find "${here}/fplinux/fixtures/${name}" -name '*.java' | LC_ALL=C sort \
		>"${work}/${name}/sources.txt"
	"${jdk}/bin/javac" -source 1.3 -target 1.3 -g:none -nowarn \
		-encoding UTF-8 -bootclasspath "${classes}" \
		-d "${work}/${name}/classes" "@${work}/${name}/sources.txt"
	"${preverify}" -classpath "${classes}" -d "${work}/${name}/preverified" \
		"${work}/${name}/classes"
	cp "${manifest}" "${work}/${name}/preverified/META-INF/MANIFEST.MF"
	"${jdk}/bin/jar" cfm "${work}/${name}.jar" "${manifest}" \
		-C "${work}/${name}/preverified" fplinux
}

fail() {
	echo "phoneME self-test: FAIL: $1" >&2
	exit 1
}

mkdir -p "${work}"
build_jar hello "${here}/hello.mf"
build_jar vectors "${here}/vectors.mf"
cp "${here}/vectors-hotspot.expected" "${work}/vectors-expected.txt"
patch -s "${work}/vectors-expected.txt" "${here}/vectors-cldc.diff"

"${qemu_arm}" "${cldc_vm}" -cp "${classes}:${work}/vectors.jar" \
	fplinux.fixtures.vectors.Vectors >"${work}/vectors-vm.txt"
cmp -s "${work}/vectors-expected.txt" "${work}/vectors-vm.txt" ||
	fail "CLDC vectors on the bare VM differ (${work}/vectors-vm.txt)"

"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 60000 "${work}/vectors.jar" >"${work}/vectors-midlet-log.txt"
# The LCDUI stub logs the MIDlet's screen changes to System.out.
grep -v '^\[lcdui\] ' "${work}/vectors-midlet-log.txt" \
	>"${work}/vectors-midlet.txt" || true
cmp -s "${work}/vectors-expected.txt" "${work}/vectors-midlet.txt" ||
	fail "CLDC vectors as a MIDlet differ (${work}/vectors-midlet.txt)"

"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 30000 "${work}/hello.jar" >"${work}/hello.txt"
grep -qx 'PM-HELLO start' "${work}/hello.txt" &&
	grep -qx 'PM-HELLO done' "${work}/hello.txt" ||
	fail "hello MIDlet output is incomplete (${work}/hello.txt)"

# The same file-backed store must survive a complete VM process exit.
build_jar rms "${here}/rms.mf"
rms_storage=$(mktemp -d "${work}/rms-storage.XXXXXX")
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--storage-dir "${rms_storage}" --timeout-ms 30000 \
	"${work}/rms.jar" >"${work}/rms-write.txt"
grep -qx 'PM-RMS write 1 2' "${work}/rms-write.txt" &&
	! grep -q '^PM-RMS error ' "${work}/rms-write.txt" ||
	fail "RMS first run differs (${work}/rms-write.txt)"
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--storage-dir "${rms_storage}" --timeout-ms 30000 \
	"${work}/rms.jar" >"${work}/rms-read.txt"
grep -qx 'PM-RMS read 1 2 next 3' "${work}/rms-read.txt" &&
	! grep -q '^PM-RMS error ' "${work}/rms-read.txt" ||
	fail "RMS second run differs (${work}/rms-read.txt)"
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--storage-dir /dev/null/blocked --timeout-ms 30000 \
	"${work}/rms.jar" fplinux.fixtures.rms.RmsErrorMIDlet >"${work}/rms-error.txt"
grep -qx 'PM-RMS storage-error' "${work}/rms-error.txt" ||
	fail "RMS storage error not reported (${work}/rms-error.txt)"

# The file protocol walks from "/". Load the ARM libraries explicitly so
# QEMU does not redirect that open to the read-only library sysroot.
build_jar files "${here}/files.mf"
files_home=$(mktemp -d "${work}/files-home.XXXXXX")
files_card=$(mktemp -d "${work}/files-card.XXXXXX")
HOME="${files_home}" "${qemu_arm}" -L / "${sysroot}/lib/ld-musl-armhf.so.1" \
	--library-path "${sysroot}/lib:${sysroot}/usr/lib" "${headless}" \
	--system-classes "${classes}" --timeout-ms 30000 \
	"${work}/files.jar" >"${work}/files.txt" 2>&1
if ! grep -qx 'PM-FILES c:/ create list read rewrite rename truncate delete' \
	"${work}/files.txt" || ! grep -qx 'PM-FILES e:/ absent' "${work}/files.txt"; then
	cat "${work}/files.txt" >&2
	fail "FileConnection behavior differs"
fi
HOME="${files_home}" "${qemu_arm}" -L / "${sysroot}/lib/ld-musl-armhf.so.1" \
	--library-path "${sysroot}/lib:${sysroot}/usr/lib" "${headless}" \
	--system-classes "${classes}" --timeout-ms 30000 --card-dir "${files_card}" \
	"${work}/files.jar" >"${work}/files-card.txt" 2>&1
if ! grep -qx 'PM-FILES c:/ create list read rewrite rename truncate delete' \
	"${work}/files-card.txt" || ! grep -qx 'PM-FILES e:/ create list read rewrite rename truncate delete' \
	"${work}/files-card.txt"; then
	cat "${work}/files-card.txt" >&2
	fail "FileConnection mounted-card behavior differs"
fi
rmdir "${files_card}" "${files_home}/Files" "${files_home}"

# Canvas port headless: sizes of the modes, sizeChanged after a mode change,
# game actions, key names and codes, GameCanvas flushes, font metrics.
build_jar canvas "${here}/canvas.mf"
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 60000 "${work}/canvas.jar" >"${work}/canvas-log.txt"
grep '^PM-CANVAS ' "${work}/canvas-log.txt" >"${work}/canvas.txt" || true
if ! cmp -s "${here}/canvas.expected" "${work}/canvas.txt"; then
	diff -u "${here}/canvas.expected" "${work}/canvas.txt" >&2 || true
	fail "canvas probe output differs"
fi

# The arrow releases before its numeric alias, then a quick fire tap
# switches to a plain Canvas and back.
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 10000 --keys '1800:-1:260,1910:50:380,2400:53:40' \
	"${work}/canvas.jar" fplinux.fixtures.canvas.GameCanvasKeysMIDlet \
	>"${work}/canvas-keys.txt"
for check in alias released tap hidden reshown; do
	if ! grep -qx "PM-GAMEKEYS ${check}=true" "${work}/canvas-keys.txt"; then
		cat "${work}/canvas-keys.txt" >&2
		fail "GameCanvas key state differs for ${check}"
	fi
done

# Each selected virtual size is stable across fullscreen mode changes.
for size in 128x128 128x160 176x208 176x220 240x320 320x240; do
	"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
		--canvas-size "${size}" --timeout-ms 30000 "${work}/canvas.jar" \
		fplinux.fixtures.canvas.CanvasSizeMIDlet >"${work}/canvas-size-${size}.txt"
	grep -qx "PM-SIZE shown ${size}" "${work}/canvas-size-${size}.txt" &&
		grep -qx "PM-SIZE full ${size} changed ${size}" "${work}/canvas-size-${size}.txt" &&
		grep -qx "PM-SIZE normal ${size} changed ${size}" "${work}/canvas-size-${size}.txt" ||
		fail "Canvas size ${size} differs (${work}/canvas-size-${size}.txt)"
done

# Form look and feel headless: the MIDP model of every Form item and TextBox
# through the real LF classes (no native peers without a runtime).
build_jar lfform "${here}/lfform.mf"
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 60000 "${work}/lfform.jar" >"${work}/lfform-log.txt"
grep '^PM-LFFORM ' "${work}/lfform-log.txt" >"${work}/lfform.txt" || true
if ! cmp -s "${here}/lfform.expected" "${work}/lfform.txt"; then
	diff "${here}/lfform.expected" "${work}/lfform.txt" | head -20 >&2 || true
	fail "Form LF check output differs (${work}/lfform.txt)"
fi

# List and Alert look and feel headless: a traced run, in which the trace
# backend of port/lf-screens models the runtime's screen stack and plays
# the scripted key presses below; "wait:" steps hold until Java has shown
# the named screen.
build_jar lfscreens "${here}/lfscreens.mf"
lfscreens_script="0:wait:Menu;0:dump;0:ok;0:wait:Pick;0:dump;0:focus:0;0:ok"
lfscreens_script="${lfscreens_script};0:cmd:Done;0:wait:Picked;0:dump;0:cmd:Close"
lfscreens_script="${lfscreens_script};0:wait:Menu;0:dump;0:focus:1;0:ok;0:wait:Multi"
lfscreens_script="${lfscreens_script};0:focus:2;0:ok;0:dump;0:cmd:Done;0:wait:Picked"
lfscreens_script="${lfscreens_script};0:cmd:Close;0:wait:Menu;0:focus:2;0:ok"
lfscreens_script="${lfscreens_script};0:wait:Gauge;0:dump;0:cmd:Close;0:wait:Menu"
lfscreens_script="${lfscreens_script};0:cmd:Info;0:wait:Info;0:dump;0:cmd:Again"
lfscreens_script="${lfscreens_script};0:wait:Info 2;0:dump;0:cmd:OK;0:wait:Menu"
lfscreens_script="${lfscreens_script};0:cmd:Edit;0:rows:4;0:dump;0:cmd:Drop;0:rows:3"
lfscreens_script="${lfscreens_script};0:dump;0:cmd:Exit"
FPLINUX_PM_LF_TRACE=1 FPLINUX_PM_LF_SCRIPT="${lfscreens_script}" \
	"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 60000 "${work}/lfscreens.jar" >"${work}/lfscreens-log.txt"
grep -E '^(PM-LFSCREENS |\[lf\] (stack|rows|keys|script)|\[lf\] alert #[0-9]+ [a-z]+ text=)' \
	"${work}/lfscreens-log.txt" >"${work}/lfscreens.txt" || true
if ! cmp -s "${here}/lfscreens.expected" "${work}/lfscreens.txt"; then
	diff "${here}/lfscreens.expected" "${work}/lfscreens.txt" | head -20 >&2 || true
	fail "List and Alert LF check output differs (${work}/lfscreens.txt)"
fi

# The native playback path must fail honestly without a UI audio provider.
# The same JAR's default AudioMIDlet requires a UI and is run separately.
build_jar media "${here}/media.mf"
"${qemu_arm}" -L "${sysroot}" "${headless}" --system-classes "${classes}" \
	--timeout-ms 30000 "${work}/media.jar" \
	fplinux.fixtures.media.HeadlessMediaMIDlet >"${work}/media-headless.txt"
grep -qx 'PM-MEDIA headless failure honest' "${work}/media-headless.txt" &&
	! grep -q '^PM-MEDIA FAIL ' "${work}/media-headless.txt" ||
	fail "headless media error state differs (${work}/media-headless.txt)"

echo "phoneME self-test: PASS (vectors, hello, RMS, files, canvas, Form, List/Alert LF and headless media)"
