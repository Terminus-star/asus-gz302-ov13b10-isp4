#!/usr/bin/env bash
# After the release tag is pushed: put the real checksum of GitHub's tag tarball into
# aur/gz302-camera-firmware/PKGBUILD (instead of SKIP) and regenerate its .SRCINFO.
#
#   ./scripts/aur-firmware-checksum.sh
#
# Also checks that the tarball holds exactly the build tools tested here (byte-identical to this
# checkout), so the AUR package builds what scripts/test-aur-firmware.sh tested.
# Download: scratch/aur-release/ (overwritten each run). Pushes nothing.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
pkgdir="$project_dir/aur/gz302-camera-firmware"
ver=$(sed -n 's/^pkgver=//p' "$pkgdir/PKGBUILD")
url="$(sed -n 's/^url="\(.*\)"$/\1/p' "$pkgdir/PKGBUILD")/archive/refs/tags/v$ver.tar.gz"	# repo URL from the PKGBUILD
dl="$project_dir/scratch/aur-release"
die() { echo "ERROR: $*" >&2; exit 1; }

mkdir -p "$dl/x"
curl -fL -o "$dl/v$ver.tar.gz" "$url" || die "download failed: $url (is tag v$ver pushed and the repo public?)"

# Same bytes as the tested files?
tar -xzf "$dl/v$ver.tar.gz" -C "$dl/x"
top=("$dl"/x/*-"$ver")	# the top folder is named after the repository
for f in tools/build_winfw.py tools/pe_emu.py tools/gen_script.py tools/gen_phy_seq.py tools/gen_sensor_prop.py; do
	cmp -s "${top[0]}/$f" "$project_dir/$f" || die "$f in the v$ver tarball differs from this checkout"
done
echo "tarball content matches the tested build tools"

sum=$(b2sum "$dl/v$ver.tar.gz" | cut -d' ' -f1)
n=$(grep -c "^b2sums=('SKIP')" "$pkgdir/PKGBUILD" || true)
[[ $n -eq 1 ]] || die "expected one b2sums=('SKIP') line in $pkgdir/PKGBUILD, found $n (already done?)"
sed -i "s|^b2sums=('SKIP').*|b2sums=('$sum')|" "$pkgdir/PKGBUILD"
bash "$project_dir/scripts/show-srcinfo.sh" "$pkgdir" > "$pkgdir/.SRCINFO"
echo "b2sums set to $sum; .SRCINFO regenerated"
