#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Same CLDC compile/preverify recipe as ../run.sh; no VM/cache writes.
# usage: FPLINUX_PM_JDK=... FPLINUX_PM_PREVERIFY=... FPLINUX_PM_CLASSES=... \
#        sh build.sh OUTPUT_DIR [http://10.0.2.2:39081]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=${1:?usage: build.sh OUTPUT_DIR [BASE_URL]}
base=${2:-http://10.0.2.2:39081}
jdk=${FPLINUX_PM_JDK:?}
preverify=${FPLINUX_PM_PREVERIFY:?}
classes=${FPLINUX_PM_CLASSES:?}
case "$base" in
http://*) ;;
*)
	echo "BASE_URL must begin with http://" >&2
	exit 1
	;;
esac
mkdir -p "$work/classes" "$work/preverified"
"$jdk/bin/javac" -source 1.3 -target 1.3 -g:none -nowarn -encoding UTF-8 \
	-bootclasspath "$classes" -d "$work/classes" \
	"$here/fplinux/fixtures/network/NetworkProbeMIDlet.java"
"$preverify" -classpath "$classes" -d "$work/preverified" "$work/classes"
# Put the property in both manifest and JAD, so direct-JAR installation works.
sed '/^NetProbe-Base-URL:/d; /^$/d' "$here/network.mf" >"$work/network.mf"
printf 'NetProbe-Base-URL: %s\n\n' "$base" >>"$work/network.mf"
"$jdk/bin/jar" cfm "$work/network.jar" "$work/network.mf" -C "$work/preverified" fplinux
sed '/^$/d' "$work/network.mf" >"$work/network.jad"
printf 'MIDlet-Jar-URL: network.jar\nMIDlet-Jar-Size: %s\n' \
	"$(wc -c <"$work/network.jar" | tr -d ' ')" >>"$work/network.jad"
echo "Network probe built: $work/network.jar"
