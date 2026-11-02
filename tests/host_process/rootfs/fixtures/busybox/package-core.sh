#!/bin/sh
set -eu

# The sourced recipe consumes both packaging directories.
# shellcheck disable=SC2034
srcdir=$2 pkgdir=$3
# shellcheck source=/dev/null
. "$1"
# The sourced recipe sets the build directory and required package metadata.
# shellcheck disable=SC2154
cd "$builddir"
package
# shellcheck disable=SC2154
printf '%s\n' "$pkgname" "$pkgver-r$pkgrel" "${provides-}" "${depends-}" "${replaces-}" >"$4"
