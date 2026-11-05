#!/usr/bin/env bash
# Ubuntu 22.04 host tools required even when vcpkg has no binary cache.
set -euo pipefail

mode="${1:---check}"
case "$mode" in
    --install)
        privilege=()
        if (( EUID != 0 )); then privilege=(sudo); fi
        "${privilege[@]}" apt-get update
        "${privilege[@]}" apt-get install --yes --no-install-recommends \
            cmake make gcc-11 g++-11 ninja-build pkg-config \
            autoconf autoconf-archive automake libtool libltdl-dev
        ;;
    --check) ;;
    *) echo 'Usage: native_prerequisites.sh [--install|--check]' >&2; exit 2 ;;
esac

missing=()
for tool in cmake make gcc-11 g++-11 ninja pkg-config autoconf autoreconf automake aclocal libtoolize; do
    if ! command -v "$tool" >/dev/null 2>&1; then missing+=("$tool"); fi
done
if (( ${#missing[@]} )); then
    printf 'Missing native host programs: %s\n' "${missing[*]}" >&2
    echo 'Run bash scripts/native_prerequisites.sh --install' >&2
    exit 1
fi

# autoconf-archive is a collection of m4 macros, not an executable. Checking
# only autoconf/aclocal misses exactly the cold libsodium build failure.
macro_dir="$(aclocal --print-ac-dir)"
for macro in ax_check_compile_flag ax_check_link_flag; do
    if [[ ! -r "$macro_dir/$macro.m4" ]]; then
        echo "Missing autoconf-archive macro: $macro_dir/$macro.m4" >&2
        echo 'Run bash scripts/native_prerequisites.sh --install' >&2
        exit 1
    fi
done
echo 'Native host programs and autoconf-archive macros verified.'
