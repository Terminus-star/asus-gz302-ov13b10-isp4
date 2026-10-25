#!/usr/bin/env bash
# Put the five Windows camera files that gz302-camera-firmware needs into ~/cam-firm-gz302ea/, taking them from an
# already MOUNTED Windows partition. Mounts nothing, writes nothing but that folder, never overwrites a file there.
#
#   ./scripts/copy-windows-files.sh            (as your user; called by scripts/install-local-packages.sh too)
#
# 1. the folder: an existing ~/cam-firm-gz302ea (any upper/lower case) or a new one
# 2. files already in it (matched case-insensitively) are kept; for each missing one it looks at every mounted
#    folder in /mnt/*, /run/media/$USER/* and /media/* for Windows/System32/DriverStore/FileRepository/
#    <package>.inf_amd64_*/<file>, walking down one level at a time, case-insensitively (no search through the
#    whole partition); with several driver versions it takes the newest one and says so
# 3. compares each file's sha256 with the versions the firmware package was tested with (read from
#    aur/gz302-camera-firmware/PKGBUILD); a different version only gives a note
# Exit status 0 = all five files present, 1 = something is still missing (with the manual instructions).
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
pkgbuild="$project_dir/aur/gz302-camera-firmware/PKGBUILD"
# same order as _files in the PKGBUILD (the tested checksums are read from there by position)
files=(OV13B_3001_ROG_SRX.atb asm_OV13B.asm srx_h_isp_firmware_signed.ram amdcamera.sys sensorOV13B.sys)
pkgs=(sensorov13b sensorov13b amdcamera amdcamera sensorov13b)	# DriverStore package of each file
mapfile -t tested < <(sed -n '/^_tested_sha256=(/,/^)/p' "$pkgbuild" | grep -o '[0-9a-f]\{64\}')
(( ${#tested[@]} == ${#files[@]} )) || { echo "ERROR: could not read the tested checksums from $pkgbuild" >&2; exit 1; }

# ci_child DIR NAME: entries directly inside DIR whose name matches the glob NAME, any upper/lower case.
ci_child() { find "$1" -mindepth 1 -maxdepth 1 -iname "$2" 2>/dev/null; }

# ---- 1. the folder ----
dir=$(ci_child "$HOME" cam-firm-gz302ea | head -n 1)
if [[ -z $dir ]]; then
	dir="$HOME/cam-firm-gz302ea"
	mkdir -p "$dir"
	echo "created $dir"
fi

# ---- 2. missing files from a mounted Windows partition ----
roots=()
for r in /mnt/* "/run/media/$USER"/* /media/*; do
	[[ -d $r ]] && roots+=("$r")
done

missing=0
for i in "${!files[@]}"; do
	f=${files[$i]}
	have=$(ci_child "$dir" "$f" | head -n 1)
	if [[ -n $have ]]; then
		echo "have   $have"
	else
		found=()
		for r in "${roots[@]}"; do
			d=$r
			for part in Windows System32 DriverStore FileRepository; do
				d=$(ci_child "$d" "$part" | head -n 1)
				[[ -n $d ]] || break
			done
			[[ -n $d ]] || continue
			while IFS= read -r p; do
				hit=$(ci_child "$p" "$f" | head -n 1)
				[[ -n $hit ]] && found+=("$hit")
			done < <(ci_child "$d" "${pkgs[$i]}.inf_amd64_*")
		done
		if (( ${#found[@]} == 0 )); then
			echo "MISSING $f (no mounted Windows partition has it)"
			missing=1
			continue
		fi
		have=$(ls -t "${found[@]}" | head -n 1)	# newest driver version
		(( ${#found[@]} > 1 )) && echo "note: ${#found[@]} copies of $f found, using the newest: $have"
		cp -- "$have" "$dir/$f"
		echo "copied $have"
		have="$dir/$f"
	fi
	# ---- 3. checksum ----
	if [[ $(sha256sum < "$have" | cut -d' ' -f1) == "${tested[$i]}" ]]; then
		echo "       same version as tested"
	else
		echo "       note: a different version from the one tested; it may still work"
	fi
done

if (( missing )); then
	cat >&2 <<EOF

Some files are still missing in $dir. Mount your Windows partition (read-only is enough; a BitLocker
partition must be unlocked first) and run this again, or copy the files by hand from
  C:\\Windows\\System32\\DriverStore\\FileRepository\\sensorov13b.inf_amd64_*\\  (OV13B_3001_ROG_SRX.atb, asm_OV13B.asm, sensorOV13B.sys)
  C:\\Windows\\System32\\DriverStore\\FileRepository\\amdcamera.inf_amd64_*\\    (srx_h_isp_firmware_signed.ram, amdcamera.sys)
EOF
	exit 1
fi
echo "all ${#files[@]} files are in $dir"
