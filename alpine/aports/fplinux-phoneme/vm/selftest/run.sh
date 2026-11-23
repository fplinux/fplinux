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

run_jit_fixture() (
	jit_mode=$1
	jit_trace=$2
	log=$3
	shift 3
	if ! FPLINUX_PHONEME_JIT=${jit_mode} FPLINUX_PHONEME_JIT_TRACE=${jit_trace} \
		"${qemu_arm}" -L "${sysroot}" "${headless}" \
		--timeout-ms 30000 "${work}/jit.jar" "$@" >"${log}" 2>&1; then
		cat "${log}" >&2
		fail "JIT fixture failed with JIT=${jit_mode} trace=${jit_trace}"
	fi
)

mkdir -p "${work}"
build_jar jit "${here}/jit.mf"
# Classes with hand-made stack maps for the verifier check below.
python3 "${here}/make-verify-classes.py" "${work}/jit/preverified/fplinux/fixtures/jit"
"${jdk}/bin/jar" uf "${work}/jit.jar" -C "${work}/jit/preverified" fplinux/fixtures/jit
run_jit_fixture '' 1 "${work}/jit-interpreted.txt"
run_jit_fixture 1 0 "${work}/jit-compiled.txt"
for output in "${work}/jit-interpreted.txt" "${work}/jit-compiled.txt"; do
	grep -qx 'PM-JIT result=-1156218545' "${output}" || {
		cat "${output}" >&2
		fail "JIT fixture checksum differs"
	}
	grep -qx 'PM-JIT wrong-remainders=0' "${output}" || {
		cat "${output}" >&2
		fail "JIT fixture produced an incorrect floating-point remainder"
	}
	# 104448 calls, each returning the width 3.
	grep -qx 'PM-JIT region-widths=313344' "${output}" || {
		cat "${output}" >&2
		fail "JIT fixture reused a folded multiply-add expression"
	}
	grep '^PM-JIT ' "${output}"
done
if grep -q 'Compiling ' "${work}/jit-interpreted.txt"; then
	fail "a method compiled in interpreted mode"
fi
# Compiler trace can interrupt MIDlet output, so inspect it separately.
run_jit_fixture 1 1 "${work}/jit-trace.txt"
awk '
    /Compiling / {
        method = /JitMIDlet\.(hotLoop|countWrongRemainders|regionWidth)/ ? $0 : ""
    }
    method && /<done [1-9][0-9]*>/ {
        print method; print
        if (method ~ /\.hotLoop/) integer = 1
        if (method ~ /\.countWrongRemainders/) remainder = 1
        if (method ~ /\.regionWidth/) region = 1
        method = ""
    }
    END { exit !(integer && remainder && region) }
' "${work}/jit-trace.txt" || {
	cat "${work}/jit-trace.txt" >&2
	fail "JIT did not compile all arithmetic methods"
}

for kind in LongMath FloatBits; do
	case ${kind} in
	LongMath)
		expected='PM-JIT-LONG hash=5564715207957261824'
		methods='multiply divideByPowersOfTwo divideByOthers'
		;;
	FloatBits)
		expected='PM-JIT-FLOATBITS hash=-976795926404499192'
		methods='floatFlows doubleFlows'
		;;
	esac
	fixture="fplinux.fixtures.jit.Jit${kind}MIDlet"
	for mode in '' 1; do
		output="${work}/jit-${kind}-${mode:-0}.txt"
		run_jit_fixture "${mode}" 0 "${output}" "${fixture}"
		grep -Fqx "${expected}" "${output}" || {
			cat "${output}" >&2
			fail "${kind} result differs with JIT=${mode:-0}"
		}
	done
	trace="${work}/jit-${kind}-trace.txt"
	run_jit_fixture 1 1 "${trace}" "${fixture}"
	awk -v fixture="Jit${kind}MIDlet" -v methods="${methods}" '
		BEGIN { n = split(methods, names, " "); for (i = 1; i <= n; i++) pending[names[i]] = 1 }
		/Compiling / {
			method = ""
			if ($(NF) ~ ("/" fixture "\\.")) {
				method = $(NF)
				sub(/^.*\./, "", method)
			}
		}
		method && /<done [1-9][0-9]*>/ { delete pending[method]; method = "" }
		END { for (name in pending) exit 1 }
	' "${trace}" || {
		cat "${trace}" >&2
		fail "JIT did not compile all ${kind} methods"
	}
done

# Stack maps past the 512 bytes of a method that the verifier keeps in its
# cache (make-verify-classes.py): one class is accepted; a class with no map
# for the instruction after a return, and classes whose maps are out of order
# or repeated, are rejected. The number in the message is the check that
# rejected the class: 10 is a missing map after a transfer of control, 33 a map
# out of order.
run_jit_fixture '' 0 "${work}/verify-maps.txt" fplinux.fixtures.jit.VerifyMapsMIDlet
for expected in \
	'PM-VERIFY VerifyLong accepted 1 2 3' \
	'PM-VERIFY VerifyNoMap rejected java.lang.Error: ClassFormatError:  10' \
	'PM-VERIFY VerifyUnordered rejected java.lang.Error: ClassFormatError:  33' \
	'PM-VERIFY VerifyDuplicate rejected java.lang.Error: ClassFormatError:  33'; do
	grep -Fqx "${expected}" "${work}/verify-maps.txt" || {
		cat "${work}/verify-maps.txt" >&2
		fail "the verifier did not give '${expected}'"
	}
done

# An exception from the inlined array access reaches the caller's handler.
for mode in '' 1; do
	output="${work}/jit-inline-${mode:-0}.txt"
	run_jit_fixture "${mode}" 0 "${output}" fplinux.fixtures.jit.JitInlineMIDlet
	grep -Fqx 'PM-JIT-INLINE handler caught=8571 null=2858 sum=132861' "${output}" || {
		cat "${output}" >&2
		fail "inlined exceptions did not reach the handler with JIT=${mode:-0}"
	}
done
run_jit_fixture 1 1 "${work}/jit-inline-trace.txt" fplinux.fixtures.jit.JitInlineMIDlet
awk '
	/Compiling / { handler = $NF ~ /\/JitInlineMIDlet\.handler$/ }
	handler && /<done [1-9][0-9]*>/ { seen = 1 }
	END { exit !seen }
' "${work}/jit-inline-trace.txt" || {
	cat "${work}/jit-inline-trace.txt" >&2
	fail "JIT did not compile the caller with an inlined exception"
}

# Hot calls from a class initializer must not recompile after each trap.
init_trace="${work}/jit-class-init.txt"
run_jit_fixture 1 1 "${init_trace}" fplinux.fixtures.jit.JitClassInitLoopMIDlet
grep -Fq 'PM-JIT-CLINIT-LOOP total=123259685' "${init_trace}" &&
	awk '
	match($0, /Compiling fplinux\/fixtures\/jit\/ClassInitLoop\$Spin\.[^[:space:]]+/) {
		method = substr($0, RSTART + 10, RLENGTH - 10)
		if (++attempts[method] > 500) repeated = 1
		seen = 1
	}
	END { exit !(seen && !repeated) }
' "${init_trace}" || fail "class initialization produced a wrong result or a recompilation loop"

build_jar hello "${here}/hello.mf"
build_jar vectors "${here}/vectors.mf"
cp "${here}/vectors-hotspot.expected" "${work}/vectors-expected.txt"
patch -s "${work}/vectors-expected.txt" "${here}/vectors-cldc.diff"

if ! timeout -k 5 60 "${qemu_arm}" "${cldc_vm}" -cp "${classes}:${work}/vectors.jar" \
	fplinux.fixtures.vectors.Vectors >"${work}/vectors-vm.txt"; then
	cat "${work}/vectors-vm.txt" >&2
	fail "CLDC vectors on the bare VM failed (${work}/vectors-vm.txt)"
fi
cmp -s "${work}/vectors-expected.txt" "${work}/vectors-vm.txt" ||
	fail "CLDC vectors on the bare VM differ (${work}/vectors-vm.txt)"

"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 60000 "${work}/vectors.jar" >"${work}/vectors-midlet-log.txt"
# The LCDUI stub logs the MIDlet's screen changes to System.out.
grep -v '^\[lcdui\] ' "${work}/vectors-midlet-log.txt" \
	>"${work}/vectors-midlet.txt" || true
cmp -s "${work}/vectors-expected.txt" "${work}/vectors-midlet.txt" ||
	fail "CLDC vectors as a MIDlet differ (${work}/vectors-midlet.txt)"

"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/hello.jar" >"${work}/hello.txt"
grep -qx 'PM-HELLO start' "${work}/hello.txt" &&
	grep -qx 'PM-HELLO done' "${work}/hello.txt" ||
	fail "hello MIDlet output is incomplete (${work}/hello.txt)"

# Query the actual MIDlet API before and after supplying a local descriptor.
build_jar jad "${here}/jad.mf"
cp "${work}/jad.jar" "${work}/jad-standalone.jar"
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/jad-standalone.jar" >"${work}/jad-standalone.txt"
grep -qx 'PM-JAD custom=null' "${work}/jad-standalone.txt" &&
	grep -qx 'PM-JAD override=manifest' "${work}/jad-standalone.txt" ||
	fail "standalone JAR properties differ (${work}/jad-standalone.txt)"

jar_size=$(stat -c %s "${work}/jad.jar")
{
	printf '\357\273\277%s\r\n' 'MIDlet-Name: Descriptor fixture'
	printf '%s\r\n' 'MIDlet-Vendor: FPLinux' 'MIDlet-Version: 1.0' \
		'MIDlet-Jar-URL: jad.jar' "MIDlet-Jar-Size: ${jar_size}" '' \
		'Fixture-Custom: 1:2' 'Fixture-Override: descriptor'
	printf 'Fixture-UTF8: \320\226\r\n'
} >"${work}/jad.jad"
for selection in jad jar; do
	output="${work}/jad-from-${selection}.txt"
	"${qemu_arm}" -L "${sysroot}" "${headless}" \
		--timeout-ms 30000 "${work}/jad.${selection}" >"${output}"
	grep -qx 'PM-JAD custom=1:2' "${output}" &&
		grep -qx 'PM-JAD override=descriptor' "${output}" &&
		grep -qx 'PM-JAD manifest=fallback' "${output}" &&
		grep -qx 'PM-JAD system=MIDP-2.0' "${output}" &&
		grep -qx 'PM-JAD utf8=true' "${output}" ||
		fail "local descriptor properties differ (${output})"
done

# The same file-backed store must survive a complete VM process exit.
build_jar rms "${here}/rms.mf"
rms_storage=$(mktemp -d "${work}/rms-storage.XXXXXX")
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--storage-dir "${rms_storage}" --timeout-ms 30000 \
	"${work}/rms.jar" >"${work}/rms-write.txt"
grep -qx 'PM-RMS write 1 2' "${work}/rms-write.txt" &&
	! grep -q '^PM-RMS error ' "${work}/rms-write.txt" ||
	fail "RMS first run differs (${work}/rms-write.txt)"
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--storage-dir "${rms_storage}" --timeout-ms 30000 \
	"${work}/rms.jar" >"${work}/rms-read.txt"
grep -qx 'PM-RMS read 1 2 next 3' "${work}/rms-read.txt" &&
	! grep -q '^PM-RMS error ' "${work}/rms-read.txt" ||
	fail "RMS second run differs (${work}/rms-read.txt)"
"${qemu_arm}" -L "${sysroot}" "${headless}" \
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
	--timeout-ms 30000 \
	"${work}/files.jar" >"${work}/files.txt" 2>&1
if ! grep -qx 'PM-FILES c:/ create list read rewrite rename truncate delete' \
	"${work}/files.txt" || ! grep -qx 'PM-FILES e:/ absent' "${work}/files.txt"; then
	cat "${work}/files.txt" >&2
	fail "FileConnection behavior differs"
fi
HOME="${files_home}" "${qemu_arm}" -L / "${sysroot}/lib/ld-musl-armhf.so.1" \
	--library-path "${sysroot}/lib:${sysroot}/usr/lib" "${headless}" \
	--timeout-ms 30000 --card-dir "${files_card}" \
	"${work}/files.jar" >"${work}/files-card.txt" 2>&1
if ! grep -qx 'PM-FILES c:/ create list read rewrite rename truncate delete' \
	"${work}/files-card.txt" || ! grep -qx 'PM-FILES e:/ create list read rewrite rename truncate delete' \
	"${work}/files-card.txt"; then
	cat "${work}/files-card.txt" >&2
	fail "FileConnection mounted-card behavior differs"
fi

HOME="${files_home}" "${qemu_arm}" -L / "${sysroot}/lib/ld-musl-armhf.so.1" \
	--library-path "${sysroot}/lib:${sysroot}/usr/lib" "${headless}" \
	--timeout-ms 15000 --card-dir "${files_card}" \
	"${work}/files.jar" fplinux.fixtures.files.FileListenerMIDlet \
	>"${work}/files-listener.txt" 2>&1 &
listener_pid=$!
for expected in ready removed; do
	attempt=0
	until grep -qx "PM-FILE-LISTENER ${expected}" "${work}/files-listener.txt"; do
		if ! kill -0 "${listener_pid}" 2>/dev/null || [ "${attempt}" -ge 100 ]; then
			kill "${listener_pid}" 2>/dev/null || true
			wait "${listener_pid}" || true
			cat "${work}/files-listener.txt" >&2
			fail "File system listener did not report ${expected}"
		fi
		attempt=$((attempt + 1))
		sleep 0.05
	done
	case "${expected}" in
	ready) mv "${files_card}" "${files_card}.removed" ;;
	removed) mv "${files_card}.removed" "${files_card}" ;;
	esac
done
wait "${listener_pid}" &&
	grep -qx 'PM-FILE-LISTENER pass' "${work}/files-listener.txt" || {
	cat "${work}/files-listener.txt" >&2
	fail "File system listener did not report the restored root"
}
rmdir "${files_card}" "${files_home}/Files" "${files_home}"

# Canvas port headless: sizes of the modes, sizeChanged after a mode change,
# game actions, key names and codes, GameCanvas draw-call smoke, font metrics.
build_jar canvas "${here}/canvas.mf"
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 60000 "${work}/canvas.jar" >"${work}/canvas-log.txt"
grep '^PM-CANVAS ' "${work}/canvas-log.txt" >"${work}/canvas.txt" || true
if ! cmp -s "${here}/canvas.expected" "${work}/canvas.txt"; then
	diff -u "${here}/canvas.expected" "${work}/canvas.txt" >&2 || true
	fail "canvas probe output differs"
fi

# The arrow releases before its numeric alias, then a quick fire tap
# switches to a plain Canvas and back.
"${qemu_arm}" -L "${sysroot}" "${headless}" \
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
	"${qemu_arm}" -L "${sysroot}" "${headless}" \
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
"${qemu_arm}" -L "${sysroot}" "${headless}" \
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
lfscreens_script="${lfscreens_script};0:left;0:wait:Picked;0:dump;0:cmd:Close"
lfscreens_script="${lfscreens_script};0:wait:Menu;0:dump;0:focus:1;0:ok;0:wait:Multi"
lfscreens_script="${lfscreens_script};0:focus:2;0:ok;0:dump;0:left;0:wait:Picked"
lfscreens_script="${lfscreens_script};0:cmd:Close;0:wait:Menu;0:focus:2;0:ok"
lfscreens_script="${lfscreens_script};0:wait:Gauge;0:dump;0:cmd:Close;0:wait:Menu"
lfscreens_script="${lfscreens_script};0:cmd:Info;0:wait:Info;0:dump;0:left"
lfscreens_script="${lfscreens_script};0:wait:Info 2;0:dump;0:cmd:OK;0:wait:Menu"
lfscreens_script="${lfscreens_script};0:cmd:Edit;0:rows:4;0:dump;0:cmd:Drop;0:rows:3"
lfscreens_script="${lfscreens_script};0:dump;0:cmd:Exit"
FPLINUX_PM_LF_TRACE=1 FPLINUX_PM_LF_SCRIPT="${lfscreens_script}" \
	"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 60000 "${work}/lfscreens.jar" >"${work}/lfscreens-log.txt"
awk -f "${here}/lfscreens-trace.awk" "${work}/lfscreens-log.txt" >"${work}/lfscreens.txt"
if ! cmp -s "${here}/lfscreens.expected" "${work}/lfscreens.txt"; then
	diff "${here}/lfscreens.expected" "${work}/lfscreens.txt" | head -20 >&2 || true
	fail "List and Alert LF check output differs (${work}/lfscreens.txt)"
fi

# The native playback path must fail honestly without a UI audio provider.
# The same JAR's default AudioMIDlet requires a UI and is run separately.
build_jar media "${here}/media.mf"
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/media.jar" \
	fplinux.fixtures.media.HeadlessMediaMIDlet >"${work}/media-headless.txt"
grep -qx 'PM-MEDIA headless failure honest' "${work}/media-headless.txt" &&
	! grep -q '^PM-MEDIA FAIL ' "${work}/media-headless.txt" ||
	fail "headless media error state differs (${work}/media-headless.txt)"

# MIDlets probing Bluetooth can catch the initialization failure and continue.
build_jar bluetooth "${here}/bluetooth.mf"
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/bluetooth.jar" >"${work}/bluetooth.txt"
grep -qx 'PM-BLUETOOTH local device unavailable' "${work}/bluetooth.txt" ||
	fail "Bluetooth availability check differs (${work}/bluetooth.txt)"

# Resources of the application JAR by name, interpreted and compiled: the
# directory entries of a JAR open as empty streams, other names must match
# exactly. The same classes run from a JAR with explicit directory entries
# and from one with files only.
build_jar jardir "${here}/jardir.mf"
mkdir -p "${work}/jardir-resources/empty"
cp -R "${here}/jardir-resources/dir" "${work}/jardir-resources/"
"${jdk}/bin/jar" uf "${work}/jardir.jar" -C "${work}/jardir-resources" dir \
	-C "${work}/jardir-resources" empty
"${jdk}/bin/jar" cfm "${work}/jardir-flat.jar" "${here}/jardir.mf" \
	-C "${work}/jardir/preverified" fplinux/fixtures/jardir/JarDirMIDlet.class \
	-C "${work}/jardir-resources" dir/b.txt -C "${work}/jardir-resources" dir/sub/a.txt
for jar in jardir jardir-flat; do
	for jit in '' 1; do
		log="${work}/${jar}-log-${jit:-0}.txt"
		if ! FPLINUX_PHONEME_JIT=${jit} "${qemu_arm}" -L "${sysroot}" "${headless}" \
			--timeout-ms 30000 "${work}/${jar}.jar" \
			>"${log}" 2>&1; then
			cat "${log}" >&2
			fail "${jar} resource fixture failed (JIT=${jit:-off})"
		fi
		grep '^PM-JARDIR ' "${log}" >"${work}/${jar}-${jit:-0}.txt" || true
		if ! cmp -s "${here}/${jar}.expected" "${work}/${jar}-${jit:-0}.txt"; then
			diff -u "${here}/${jar}.expected" "${work}/${jar}-${jit:-0}.txt" >&2 || true
			fail "${jar} resource lookups differ (JIT=${jit:-off})"
		fi
	done
done

for api in wireless jsr; do
	build_jar "${api}" "${here}/${api}.mf"
	"${qemu_arm}" -L "${sysroot}" "${headless}" \
		--timeout-ms 30000 "${work}/${api}.jar" >"${work}/${api}-log.txt" 2>&1
	grep '^PM-' "${work}/${api}-log.txt" >"${work}/${api}.txt" || true
	if ! cmp -s "${here}/${api}.expected" "${work}/${api}.txt"; then
		cat "${work}/${api}-log.txt" >&2
		fail "${api} API compatibility check differs"
	fi
done

build_jar vendor "${here}/vendor.mf"
mkdir -p "${work}/vendor-resources"
for asset in pixel.png tone.mid; do
	base64 -d "${here}/vendor-resources/${asset}.base64" >"${work}/vendor-resources/${asset}"
done
"${jdk}/bin/jar" uf "${work}/vendor.jar" -C "${work}/vendor-resources" .
"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/vendor.jar" >"${work}/vendor-log.txt" 2>&1
if ! grep -qx 'PM-VENDOR checks=ok' "${work}/vendor-log.txt"; then
	cat "${work}/vendor-log.txt" >&2
	fail "vendor API compatibility check differs"
fi

# Model a host wake before the existing Java deadline, with no event that
# would make another Java thread runnable.
build_jar scheduler "${here}/scheduler.mf"
FPLINUX_PHONEME_JIT=1 FPLINUX_PM_VM_TEST_EARLY=1 \
	"${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/scheduler.jar" >"${work}/scheduler-log.txt" 2>&1
grep -E '^(PM-VM forced-early |PM-SLEEP )' "${work}/scheduler-log.txt"
awk '
/^PM-VM forced-early / {
	split($4, minimum, "=")
	split($5, maximum, "=")
	split($6, result, "=")
	# Both VM clock reads can occur anywhere within their respective calls.
	if (minimum[2] < 35)
		next
	early++
	if (result[2] < minimum[2] - 2 || result[2] > maximum[2] + 2)
		wrong++
}
/^PM-SLEEP frame=/ { frames++ }
END {
	printf "PM-VM deadline check early=%d wrong=%d frames=%d\n",
		early, wrong, frames
	exit !(early >= 8 && wrong == 0 && frames == 12)
}
' "${work}/scheduler-log.txt" ||
	fail "early VM slice changed the pending Java deadline"

# System.gc(): a weak reference to an unreachable object is cleared whether
# the object is young or old, freeMemory() reflects a collection once the
# heap is nearly full, and a call that cannot free anything a program can
# observe is skipped.
build_jar gc "${here}/gc.mf"
for heap in 4096 8192; do
	for mode in '' 1; do
		output="${work}/gc-${heap}-${mode:-0}.txt"
		if ! FPLINUX_PHONEME_JIT=${mode} "${qemu_arm}" -L "${sysroot}" "${headless}" \
			--heap-kib "${heap}" --timeout-ms 30000 \
			"${work}/gc.jar" >"${output}" 2>&1; then
			cat "${output}" >&2
			fail "System.gc() fixture failed with heap ${heap} KiB and JIT=${mode:-0}"
		fi
		for expected in 'PM-GC weak-young=8/8' 'PM-GC weak-old=8/8' \
			'PM-GC free-after-low-memory=true' 'PM-GC idle-call-skipped=true'; do
			grep -Fqx "${expected}" "${output}" || {
				cat "${output}" >&2
				fail "System.gc() fixture: no '${expected}' with heap ${heap} KiB and JIT=${mode:-0}"
			}
		done
	done
done

# A clock wait keeps its deadline and allows a runnable peer to progress.
build_jar clock "${here}/clock.mf"
FPLINUX_PHONEME_JIT=1 "${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/clock.jar" \
	>"${work}/clock-log.txt" 2>&1
grep -E '^PM-CLOCK ' "${work}/clock-log.txt"
grep -qx 'PM-CLOCK backwards=0' "${work}/clock-log.txt" &&
	grep -qx 'PM-CLOCK counter-progress=true' "${work}/clock-log.txt" &&
	awk '
	/^PM-CLOCK late / {
		for (i = 3; i <= NF; i++) {
			split($i, field, "=")
			late[field[1]] = field[2]
		}
		seen++
	}
	/^PM-CLOCK sleeper-late=/ { split($2, field, "="); sleeper = field[2]; sleeping++ }
	END {
		# Deadlines within 5 ms; with a second thread that has work to do
		# the time slices of the threads show up, as with the previous VM.
		exit !(seen == 1 && late["busy"] <= 5 && late["yield"] <= 5 &&
			late["with-sleeper"] <= 5 && late["with-counter"] <= 25 &&
			sleeping == 1 && sleeper <= 25)
	}
' "${work}/clock-log.txt" || {
	cat "${work}/clock-log.txt" >&2
	fail "clock wait loops missed a deadline, went backwards or starved a thread"
}
FPLINUX_PHONEME_JIT=1 "${qemu_arm}" -L "${sysroot}" "${headless}" \
	--timeout-ms 30000 "${work}/clock.jar" \
	fplinux.fixtures.clock.ClockSpinMIDlet >"${work}/clock-spin.txt" 2>&1
grep -Eq '^PM-CLOCK spin late-max=[0-5] backwards=0$' "${work}/clock-spin.txt" || {
	cat "${work}/clock-spin.txt" >&2
	fail "the single-threaded clock wait loop missed a deadline or went backwards"
}

echo "phoneME self-test: PASS (vectors, hello, local JAD, RMS, files, canvas, Form, List/Alert LF, headless media, Bluetooth availability, JAR directory entries, Java deadlines, System.gc() and clock waits)"
