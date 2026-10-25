#!/usr/bin/env bash
# Add new out-of-tree driver patches (driver/patches/00NN-*.patch) to the DKMS package's capture patch
# 0007-media-amd-isp4-gz302-capture.patch (in $patch_dir below), for the kernel version in the tarball.
#
#   ./scripts/update-capture-patch.sh TARBALL FIRST LAST
#   e.g. ./scripts/update-capture-patch.sh kernel/build/tarballs/cachyos-7.2.11-1.tar.gz 38 46
#
# Why stack instead of re-squashing from the 7.2.0 base of driver/? The current 0007 is already refreshed for
# the newer kernel, where upstream changed some of the same functions (error paths in isp4_subdev.c), and a
# fresh squash from 7.2.0 no longer applies there. Stacking keeps upstream's changes as they are.
#
# Steps (stops at the first problem; 0007 is only overwritten at the very end):
#   1. extract the pristine isp4 driver from TARBALL into scratch/capture-update/run.XXXXXX/{pristine,tree}/
#      (a new folder each run; nothing is deleted, old run folders can be removed by hand)
#   2. apply the current 0007 to tree/ with -F0 (no fuzz: every context line must match)
#   3. apply driver/patches/00FIRST..00LAST to tree/ in order, also -F0 (line offsets are fine, fuzz is not)
#   4. write the new 0007: header (patch range updated) + one diff per changed file, pristine vs tree
# Afterwards: raise dkms_ver in scripts/make-aur-dkms.sh, run it (it builds the package from these patches), then
# scripts/install-local-packages.sh.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tarball=${1:?usage: $0 TARBALL FIRST LAST}
first=${2:?usage: $0 TARBALL FIRST LAST}
last=${3:?usage: $0 TARBALL FIRST LAST}
kpath=drivers/media/platform/amd/isp4
patch_dir="$project_dir/dkms/patches"	# the DKMS package's patches against the CachyOS sources
target="$patch_dir/0007-media-amd-isp4-gz302-capture.patch"
die() { echo "ERROR: $*" >&2; exit 1; }

# ---- 1. pristine copies ----
mkdir -p "$project_dir/scratch/capture-update"
work=$(mktemp -d -p "$project_dir/scratch/capture-update" run.XXXXXX)
echo "work folder: $work"
top=$(basename "$tarball" .tar.gz)	# e.g. cachyos-7.2.11-1, also the tarball's top folder
mkdir -p "$work/pristine" "$work/tree"
tar -xzf "$tarball" -C "$work/pristine" --strip-components=1 "$top/$kpath"
cp -r "$work/pristine/." "$work/tree/"

# ---- 2. current 0007 ----
patch --batch --no-backup-if-mismatch -s -p1 -F0 -d "$work/tree" < "$target" || die "current 0007 does not apply cleanly to $top"

# ---- 3. new driver patches, in order ----
for n in $(seq "$first" "$last"); do
	p=$(ls "$project_dir"/driver/patches/"$(printf '%04d' "$n")"-*.patch 2>/dev/null) || die "no driver patch $n"
	echo "applying $(basename "$p")"
	# --batch: never ask questions; a dry run first so a failing patch leaves tree/ untouched.
	patch --dry-run --batch -s -p1 -F0 -d "$work/tree" < "$p" || die "$(basename "$p") needs fuzz or fails"
	patch --batch --no-backup-if-mismatch -s -p1 -F0 -d "$work/tree" < "$p"
done

# ---- 4. new 0007 ----
new="$work/0007.new"
# Header = everything up to the first "---" line, with the squashed range updated (0009-NN).
sed -n '1,/^---$/p' "$target" | sed "s/patches 0009-[0-9]*/patches 0009-$(printf '%04d' "$last")/" > "$new"
# One line about the focus control (0043/0045), added once, after the firmware-files sentence.
grep -q 'focus_absolute' "$new" ||
	sed -i '/gz302-ov13b-\*\.bin\.$/a Manual focus as the V4L2 control focus_absolute, written to the focus motor\nthrough the firmware (SEND_I2C_MSG) while streaming.' "$new"
for f in "$work/tree/$kpath"/*; do
	name=$(basename "$f")
	case $name in *.orig|*.rej) continue ;; esac
	cmp -s "$work/pristine/$kpath/$name" "$f" && continue
	echo "diff --git a/$kpath/$name b/$kpath/$name" >> "$new"
	diff -u --label "a/$kpath/$name" --label "b/$kpath/$name" \
		"$work/pristine/$kpath/$name" "$f" >> "$new" || [[ $? -eq 1 ]]
done
# Self-check before overwriting: the new patch must apply to pristine with -F0.
patch --dry-run -s -p1 -F0 -d "$work/pristine" < "$new" || die "new 0007 does not apply to $top"
cp "$new" "$target"
echo "wrote $target"
