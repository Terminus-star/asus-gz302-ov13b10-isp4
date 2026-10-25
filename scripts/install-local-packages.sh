#!/usr/bin/env bash
# Build and install the two camera packages from aur/ on this machine, so pacman tracks them, without the AUR:
#   1. gz302-camera-firmware   (needs the five Windows files in ~/cam-firm-gz302ea/; copied automatically from a
#                               mounted Windows partition by scripts/copy-windows-files.sh)
#   2. gz302-camera-dkms       (camera drivers as source, built by DKMS for each linux-cachyos kernel, plus the
#                               ACPI camera table that lets the stock kernel find the camera; findings 136-138)
# Runs on the stock linux-cachyos kernel: no kernel build is needed.
#
#   ./scripts/install-local-packages.sh             (as your user; asks for sudo when installing)
#   ./scripts/install-local-packages.sh --rebuild   (build the packages again even if their files exist)
#   ./scripts/install-local-packages.sh --dry-run   (only print what it would do; builds and installs nothing)
#
# Order matters: gz302-camera-dkms depends on gz302-camera-firmware, so the firmware package goes first.
# Installing either package makes pacman's mkinitcpio hook rebuild the initramfs of every installed kernel
# (the ISP firmware image and the ACPI table must be in each of them); installing the DKMS package also builds
# the modules for every installed linux-cachyos kernel that has its headers (DKMS's pacman hooks), and for
# newer kernels later, when their headers arrive.
#
# Safe to run again; each step only does what is still missing:
#   - a package already installed at the version in aur/ is skipped (no rebuild, no reinstall)
#   - packages already built for that version (e.g. the sudo password prompt timed out) are installed from
#     kernel/build/local/ without building again
#
# Before building anything it checks the prerequisites and lists every problem at once:
#   - the five Windows files in ~/cam-firm-gz302ea/ (any upper/lower case), if the firmware package needs
#     building; missing ones are copied from a mounted Windows partition (scripts/copy-windows-files.sh)
#   - none of the files the firmware package installs may already exist without belonging to it
#     (e.g. copies made by hand); pacman would refuse with "exists in filesystem"
#   - the running kernel's headers must be installed (DKMS builds against them)
# Then it stops at the first failure. pacman shows what it will do and asks before installing.
# Build folders: kernel/build/local/<package>/ (git-ignored); downloads go to kernel/build/tarballs/ (SRCDEST).
# A built package is reused only while the version in aur/ stays the same, so any change to a package
# must come with a new pkgver or pkgrel.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
die() { echo "ERROR: $*" >&2; exit 1; }
[[ $EUID -ne 0 ]] || die "run as your user, not root (makepkg refuses root)"

rebuild=0
dry=0
for arg in "$@"; do	# options can be combined, e.g. --rebuild --dry-run
	case $arg in
		--rebuild) rebuild=1 ;;
		--dry-run) dry=1 ;;
		*) die "unknown option: $arg (--rebuild or --dry-run)" ;;
	esac
done

# Version in aur/NAME (pkgver-pkgrel, as pacman -Q prints it; these packages have no epoch).
aur_version() {
	local s="$project_dir/aur/$1/.SRCINFO"
	echo "$(sed -n 's/^\tpkgver = //p' "$s")-$(sed -n 's/^\tpkgrel = //p' "$s")"
}
# Installed version of a package, or nothing.
installed_version() { pacman -Q "$1" 2>/dev/null | cut -d' ' -f2 || true; }

fw_ver=$(aur_version gz302-camera-firmware)
dk_ver=$(aur_version gz302-camera-dkms)
fw_needed=1; [[ $(installed_version gz302-camera-firmware) == "$fw_ver" ]] && fw_needed=0
dk_needed=1; [[ $(installed_version gz302-camera-dkms) == "$dk_ver" ]] && dk_needed=0
(( rebuild )) && { fw_needed=1; dk_needed=1; }
(( dk_needed || fw_needed )) || { echo "gz302-camera-firmware $fw_ver and gz302-camera-dkms $dk_ver are already installed; nothing to do."; exit 0; }

# ---- prerequisites ----
problems=0
# The five Windows files: scripts/copy-windows-files.sh keeps what is in ~/cam-firm-gz302ea/ and copies anything
# missing from a mounted Windows partition (it mounts nothing); it prints the manual steps if that is not enough.
if (( fw_needed )) && ! bash "$project_dir/scripts/copy-windows-files.sh"; then
	problems=1
fi
# The firmware package generates two of its files by running functions of the Windows driver files in an emulator
# (tools/gen_phy_seq.py, tools/gen_sensor_prop.py); makepkg runs without -s here, so check its build dependency.
if (( fw_needed )) && ! pacman -Q python-unicorn > /dev/null 2>&1; then
	echo "MISSING: python-unicorn (CPU emulator for the firmware build); install it:  sudo pacman -S python-unicorn" >&2
	problems=1
fi
if (( fw_needed )); then
	for f in /usr/lib/firmware/updates/amdgpu/isp_4_1_1.bin \
		/usr/lib/firmware/amdgpu/gz302-ov13b-script.bin \
		/usr/lib/firmware/amdgpu/gz302-ov13b-phy-seq.bin \
		/usr/lib/firmware/amdgpu/gz302-ov13b-sensor-prop.bin \
		/usr/lib/firmware/amdgpu/OV13B_3001_ROG_SRX.atb; do
		[[ -e $f ]] || continue
		owner=$(pacman -Qqo "$f" 2>/dev/null || true)
		if [[ $owner != gz302-camera-firmware ]]; then
			echo "CONFLICT: $f exists and belongs to ${owner:-no package}; remove it first:  sudo rm $f" >&2
			problems=1
		fi
	done
fi
# DKMS builds against /usr/lib/modules/<kernel>/build, which the -headers package provides.
if (( dk_needed )) && [[ ! -f /usr/lib/modules/$(uname -r)/build/.config ]]; then
	# The headers package is named after the kernel package that owns the running kernel's vmlinuz
	# (linux-cachyos -> linux-cachyos-headers, linux-zen -> linux-zen-headers, ...).
	kpkg=$(pacman -Qqo "/usr/lib/modules/$(uname -r)/vmlinuz" 2>/dev/null || true)
	if [[ -n $kpkg ]]; then
		echo "MISSING: headers for the running kernel $(uname -r); install them:  sudo pacman -S $kpkg-headers" >&2
	else
		echo "MISSING: headers for the running kernel $(uname -r); install its -headers package" >&2
	fi
	problems=1
fi
(( problems == 0 )) || die "fix the problems above, then run this again (nothing was built or installed)"

export SRCDEST="$project_dir/kernel/build/tarballs"
mkdir -p "$SRCDEST"

# build_and_install NAME: copy aur/NAME into its build folder; build there unless every package file for this
# version already exists (or --rebuild); then install them.
build_and_install() {
	local name=$1 b="$project_dir/kernel/build/local/$1" p have=1
	local -a pkgs
	mkdir -p "$b"
	cp -r "$project_dir/aur/$name/." "$b/"
	mapfile -t pkgs < <(cd "$b" && makepkg --packagelist)
	for p in "${pkgs[@]}"; do [[ -f $p ]] || have=0; done
	if (( dry )); then
		if (( have && !rebuild )); then echo "DRY RUN: $name: would install the packages built earlier:"
		else echo "DRY RUN: $name: would build in $b, then install:"; fi
		printf '  %s\n' "${pkgs[@]}"
		return 0
	fi
	if (( have && !rebuild )); then
		echo "=== $name: using the packages built earlier; --rebuild to build again"
	else
		echo "=== building $name in $b"
		(cd "$b" && makepkg -f) || die "makepkg failed for $name (see above)"
		for p in "${pkgs[@]}"; do [[ -f $p ]] || die "expected package missing: $p"; done
	fi
	echo "=== installing:"; printf '  %s\n' "${pkgs[@]}"
	sudo pacman -U "${pkgs[@]}" \
		|| die "pacman -U failed or was cancelled for $name. Run this script again: it installs the built packages without building."
}

if (( fw_needed )); then
	build_and_install gz302-camera-firmware
else
	echo "=== gz302-camera-firmware $fw_ver already installed; skipped"
fi
if (( dk_needed )); then
	# Sources only, so this takes seconds; pacman's DKMS hook then compiles the modules (about a minute).
	build_and_install gz302-camera-dkms
else
	echo "=== gz302-camera-dkms $dk_ver already installed; skipped"
fi
(( dry )) && { echo "DRY RUN: nothing was built or installed."; exit 0; }

cat <<'EOF'

Done. Reboot (the ACPI camera table and the ISP firmware are loaded at boot), then check:
  dkms status gz302-camera                              (installed for your kernel)
  modinfo -n amd_isp4_capture ov13b10 amd_isp4          (paths under updates/dkms/)
  cat /sys/bus/acpi/devices/OMNI13B1:00/modalias        (ends in :OMNI5C10:)
  ls /dev/video*
  ffplay -f v4l2 -video_size 1920x1080 -input_format nv12 /dev/video0
EOF
