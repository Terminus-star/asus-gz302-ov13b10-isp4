#!/usr/bin/env bash
# Copy the two AUR packages (gz302-camera-firmware, gz302-camera-dkms) from aur/ into local clones of their AUR
# git repositories, ready for you
# to commit and push. Pushes nothing itself.
#
#   ./scripts/aur-sync.sh
#
# Needs an AUR account with your SSH key (https://aur.archlinux.org/account). Cloning a name that does
# not exist yet gives an empty repository; the first push creates the package on the AUR.
# Clones live in aur-git/ (git-ignored). Refuses when the firmware checksum is still SKIP or a .SRCINFO
# does not match its PKGBUILD (the AUR and paru read .SRCINFO, not the PKGBUILD).
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }

grep -q "^b2sums=('SKIP')" "$project_dir/aur/gz302-camera-firmware/PKGBUILD" \
	&& die "firmware b2sums is still SKIP: push the tag, then run scripts/aur-firmware-checksum.sh"

for pkg in gz302-camera-firmware gz302-camera-dkms; do
	src="$project_dir/aur/$pkg"
	dst="$project_dir/aur-git/$pkg"

	# .SRCINFO up to date? Regenerate into the clone folder's parent and compare.
	mkdir -p "$project_dir/aur-git"
	bash "$project_dir/scripts/show-srcinfo.sh" "$src" > "$project_dir/aur-git/$pkg.SRCINFO.check"
	cmp -s "$project_dir/aur-git/$pkg.SRCINFO.check" "$src/.SRCINFO" \
		|| die "$src/.SRCINFO is out of date (regenerate it from the PKGBUILD)"

	[[ -d $dst/.git ]] || git clone "ssh://aur@aur.archlinux.org/$pkg.git" "$dst"

	# Only the files the package needs: PKGBUILD, .SRCINFO and its local sources. The DKMS package's sources
	# (driver files, Kbuild, dkms.conf, ACPI table, drop-in, .install) all live next to its PKGBUILD. A file
	# dropped from the package is NOT removed from the clone; git status below shows it.
	cp "$src/PKGBUILD" "$src/.SRCINFO" "$dst/"
	if [[ $pkg == gz302-camera-dkms ]]; then
		cp "$src"/* "$dst/"
	fi
	echo "=== $pkg ($dst)"
	git -C "$dst" status --short
done

ver_fw=$(sed -n 's/^pkgver=//p' "$project_dir/aur/gz302-camera-firmware/PKGBUILD")
ver_dk=$(sed -n 's/^pkgver=//p' "$project_dir/aur/gz302-camera-dkms/PKGBUILD")
cat <<EOF

Review the changes above, then publish each package yourself:
  git -C aur-git/gz302-camera-firmware add -A
  git -C aur-git/gz302-camera-firmware commit -m "gz302-camera-firmware $ver_fw"
  git -C aur-git/gz302-camera-firmware push origin master
  git -C aur-git/gz302-camera-dkms add -A
  git -C aur-git/gz302-camera-dkms commit -m "gz302-camera-dkms $ver_dk"
  git -C aur-git/gz302-camera-dkms push origin master
EOF
