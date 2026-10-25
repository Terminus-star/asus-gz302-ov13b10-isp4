#!/usr/bin/env bash
# Print what makepkg makes of a PKGBUILD (package names, version, sources and their checksums)
# without downloading or building anything.
#
#   ./scripts/show-srcinfo.sh PKGBUILD_DIR
#
# makepkg only accepts a PKGBUILD in the current directory, hence the cd (inside this script only).
# Note: this evaluates the PKGBUILD's top-level shell code, as every makepkg call does.
set -euo pipefail
dir=${1:?usage: $0 PKGBUILD_DIR}
cd -- "$dir"
makepkg --printsrcinfo
