#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
revision="$(tr -d '\r\n' < "$root/cmake/vcpkg-revision.txt")"
[[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid pinned vcpkg revision' >&2; exit 1; }
if [[ ! -d "$root/vcpkg/.git" && ! -f "$root/vcpkg/.git" ]]; then
    git clone https://github.com/microsoft/vcpkg.git "$root/vcpkg"
fi
if ! git -C "$root/vcpkg" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$root/vcpkg" fetch origin "$revision"
fi
git -C "$root/vcpkg" checkout --detach "$revision"
python3 "$root/scripts/check_vcpkg_registry.py"
bash "$root/vcpkg/bootstrap-vcpkg.sh" -disableMetrics
