#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Host toolchain for the phoneME generators, installed as gcc, g++, as and ar
# in the generator tool directory. loopgen, romgen and preverify must be ILP32
# programs (they lay out ARM target structures with host types), so they are
# built as static i586 musl programs against FPLINUX_PM_I386_ROOT. The build
# sandbox rejects native i386 system calls, so a linked program is written as
# NAME.elf and NAME becomes a script that runs it through qemu-i386.
set -eu

i386_root=${FPLINUX_PM_I386_ROOT:?}
resource_dir=${FPLINUX_PM_I386_RESOURCE_DIR:?}
qemu_i386=${FPLINUX_PM_QEMU_I386:?}
target=i586-alpine-linux-musl
tool=${0##*/}

case ${tool} in
as) exec /usr/bin/as --32 "$@" ;;
ar) exec /usr/bin/ar "$@" ;;
g++ | c++)
	driver=clang++
	cxx_include=${i386_root}/usr/include/c++/15.2.0
	set -- -nostdinc++ -isystem "${cxx_include}" \
		-isystem "${cxx_include}/${target}" \
		-std=gnu++98 -Wno-register -Wno-c++11-narrowing "$@"
	;;
gcc | cc) driver=clang ;;
*)
	echo "host-cc.sh: unknown tool name ${tool}" >&2
	exit 2
	;;
esac

# A command without -c, -S, -E or -M links a generator.
links=true
output=
expect_output=false
for argument; do
	if ${expect_output}; then
		output=${argument}
		expect_output=false
		continue
	fi
	case ${argument} in
	-c | -S | -E | -M | -MM) links=false ;;
	-o) expect_output=true ;;
	*) ;;
	esac
done

# The upstream host rules add -m32, which would change the clang target, and
# the GCC-only -fno-optional-diags.
expect_output=false
for argument; do
	shift
	if ${expect_output}; then
		expect_output=false
		if ${links}; then
			set -- "$@" "${argument}.elf"
			continue
		fi
	fi
	case ${argument} in
	-m32 | -fno-optional-diags) continue ;;
	-o) expect_output=true ;;
	*) ;;
	esac
	set -- "$@" "${argument}"
done

# clang -O1 and -O2 miscompile the generators' handle lists: romgen asserts
# "handle not on stack" while romizing the MIDP classes. These flags must
# follow the -O2 and -fstrict-aliasing that the build adds.
set -- --target="${target}" --sysroot="${i386_root}" \
	-resource-dir="${resource_dir}" "$@" -O0 -fno-strict-aliasing
if ! ${links} || [ -z "${output}" ]; then
	exec "${driver}" "$@"
fi

"${driver}" -static -rtlib=compiler-rt -unwindlib=libunwind -fuse-ld=lld "$@"
elf_directory=$(cd "$(dirname "${output}")" && pwd)
elf=${elf_directory}/${output##*/}.elf
printf '#!/bin/sh\nexec %s %s "$@"\n' "${qemu_i386}" "${elf}" >"${output}"
chmod 755 "${output}"
