#!/usr/bin/env bash
# Download a package's sources exactly as makepkg would and check their checksums and signatures,
# without building: catches a wrong b2sum or a missing release tag before anyone else hits it.
#
#   ./scripts/verify-sources.sh aur/gz302-camera-firmware
#
# Works on a copy in scratch/verify/NAME/ (overwritten each run); downloads land there too.
# makepkg needs the PKGBUILD's folder as current directory, hence the cd in a subshell.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$(cd -- "${1:?usage: $0 PACKAGE_DIR}" && pwd)
work="$project_dir/scratch/verify/$(basename "$src")"

mkdir -p "$work"
cp -r "$src/." "$work/"
(cd "$work" && makepkg --verifysource -f)
