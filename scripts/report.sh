#!/usr/bin/env bash
# Write a short status report of the GZ302 rear camera, to attach to a GitHub issue or a mailing-list reply.
#
#   ./scripts/report.sh            (as your user, no sudo)
#   ./scripts/report.sh --check    (also runs scripts/camera-check.sh: a 3 s capture; close OBS/browsers first)
#
# Contains: kernel and package versions, board name and BIOS version, DKMS status, the camera's ACPI IDs, which
# module files are loaded, the video nodes, sha256 of the installed camera firmware files, and the camera lines of
# this boot's kernel log. Leaves out: hostname, timestamps, serial numbers, your home folder and the clone's path
# (shown as ~ and <repo>), the Windows files themselves, firmware or calibration contents. Read it before you share it.
# Output: scratch/reports/gz302-camera-report-STAMP.txt (scratch/ is not tracked by git).
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
check=0
case ${1:-} in
	"") ;;
	--check) check=1 ;;
	*) echo "ERROR: unknown option $1 (only --check)" >&2; exit 1 ;;
esac
out="$project_dir/scratch/reports"
mkdir -p "$out"
report="$out/gz302-camera-report-$(date +%Y%m%d-%H%M%S).txt"

section() { printf '\n== %s ==\n' "$1"; }

{
	echo "GZ302 rear camera report (asus-gz302-ov13b10-isp4 scripts/report.sh)"
	section "system"
	echo "kernel:  $(uname -r)"
	echo "product: $(</sys/class/dmi/id/product_name)"
	echo "board:   $(</sys/class/dmi/id/board_name)"
	echo "bios:    $(</sys/class/dmi/id/bios_version) ($(</sys/class/dmi/id/bios_date))"
	echo "repo:    $(git -C "$project_dir" describe --always --dirty 2>/dev/null || echo "not a git checkout")"

	section "packages"
	for p in gz302-camera-dkms gz302-camera-firmware linux-cachyos linux-cachyos-headers dkms mkinitcpio linux-firmware-amdgpu; do
		pacman -Q "$p" 2>/dev/null || echo "$p: not installed"
	done

	section "dkms"
	dkms status gz302-camera 2>&1 || true

	section "acpi"
	echo "modalias: $(cat /sys/bus/acpi/devices/OMNI13B1:00/modalias 2>/dev/null || echo "no OMNI13B1 device")   (expected acpi:OMNI13B1:OMNI5C10:)"

	section "modules"
	for m in amd_isp4_capture ov13b10 amd_isp4; do
		loaded=no; grep -q "^$m " /proc/modules && loaded=yes
		echo "$m: loaded=$loaded file=$(modinfo -n "$m" 2>/dev/null || echo "?")"
	done

	section "video nodes"
	for n in /sys/class/video4linux/video*; do
		[[ -e $n ]] && echo "/dev/${n##*/}: $(<"$n/name")"
	done

	section "firmware files (sha256)"
	for f in /usr/lib/firmware/updates/amdgpu/isp_4_1_1.bin /usr/lib/firmware/amdgpu/gz302-ov13b-script.bin \
		/usr/lib/firmware/amdgpu/gz302-ov13b-phy-seq.bin /usr/lib/firmware/amdgpu/gz302-ov13b-sensor-prop.bin \
		/usr/lib/firmware/amdgpu/OV13B_3001_ROG_SRX.atb; do
		if [[ -e $f ]]; then echo "$(sha256sum < "$f" | cut -d' ' -f1)  $f"; else echo "missing  $f"; fi
	done

	if (( check )); then
		section "camera check"
		# camera-check prints full paths of its result files; hide the clone's location and the home folder (user name)
		bash "$project_dir/scripts/camera-check.sh" 2>&1 \
			| sed -e "s|$project_dir|<repo>|g" -e "s|$HOME|~|g" \
			|| echo "camera-check failed (see above)"
	fi

	section "kernel log, camera lines of this boot (firmware log lines left out)"
	journalctl -k -b 0 --no-pager -o cat 2>/dev/null \
		| grep -E "isp_v4_1_1|amd_isp|amdisp|AMDISP|ov13b10|OMNI13B1|CAMBCID" \
		| grep -v "EXP-FWLOG" | head -n 300 || true
} > "$report" 2>&1

echo "wrote $report ($(wc -l < "$report") lines); read it before you share it"
