#!/usr/bin/env bash
# Lens sweep without any firmware AF: measure image sharpness at fixed focus-motor positions, to check the
# direction and scale of the motor code before trusting the firmware's AF position mapping (findings 128).
#
#   ./scripts/focus-sweep.sh [POSITION...]     default: 64 175 235 330 450 555 655 800 1000
#
# Point the camera at ONE still, detailed scene at a known distance (write it down) and do not move it.
# For each position: set focus_absolute (stored by the driver while not streaming and written to the motor
# at sensor start, before the sensor streams: patch 0043), capture 40 frames, measure the mean sharpness of
# frames 30-39 (auto exposure has settled by then) with tools/focus_metric.py.
# Uses driver/amd_isp4_capture.ko with its module defaults (focus_control=1, focus_via_fw=1, no AF drive).
# Output: scratch/focus-sweep/STAMP-posN.nv12 and .log (kernel log), STAMP-summary.txt. Unloads the test module
# at the end and loads the installed one again, like scripts/test-isp4-threshold.sh.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ko="$project_dir/driver/amd_isp4_capture.ko"
out="$project_dir/scratch/focus-sweep"
positions=("$@")
[[ ${#positions[@]} -gt 0 ]] || positions=(64 175 235 330 450 555 655 800 1000)
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ -f $ko ]] || die "build the module first: ./scripts/build-isp4-threshold.sh"
[[ $(modinfo -F vermagic "$ko" | cut -d' ' -f1) == "$(uname -r)" ]] || die "$ko is not built for $(uname -r)"
command -v v4l2-ctl >/dev/null || die "v4l2-ctl not found (package v4l-utils)"
mkdir -p "$out"
stamp=$(date +%Y%m%d-%H%M%S)
summary="$out/$stamp-summary.txt"
sudo -n -l >/dev/null 2>&1 || sudo -v || die "sudo is needed"

# Same dependency loading and module swap as scripts/test-isp4-threshold.sh.
sudo modprobe -a mc videodev videobuf2-common videobuf2-v4l2 videobuf2-vmalloc || die "could not load dependencies"
if grep -q '^amd_isp4_capture ' /proc/modules; then
	sudo rmmod amd_isp4_capture || die "cannot unload amd_isp4_capture (in use? close OBS/browsers)"
fi
sudo insmod "$ko" dyndbg=+p || die "insmod failed; see: sudo dmesg | tail -30"
sleep 2

# The ISP node: the video device whose name or sysfs path mentions "isp" (not the USB front camera).
node=""
for n in /sys/class/video4linux/video*; do
	name=$(<"$n/name")
	path=$(readlink -f "$n/device" 2>/dev/null || true)
	if [[ ${name,,} == *isp* || ${path,,} == *isp* ]]; then node="/dev/${n##*/}"; break; fi
done
[[ -n $node ]] || die "no ISP video node found"
echo "ISP node $node; positions: ${positions[*]}" | tee "$summary"

for pos in "${positions[@]}"; do
	cap="$out/$stamp-pos$pos.nv12"
	cursor=$(sudo journalctl -k -n 0 --show-cursor --no-pager | sed -n 's/^-- cursor: //p')
	v4l2-ctl -d "$node" --set-ctrl focus_absolute="$pos"
	timeout 10s v4l2-ctl -d "$node" \
		--set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
		--stream-mmap=4 --stream-count=40 --stream-to="$cap" --stream-poll || true
	sleep 1
	sudo journalctl -k --after-cursor "$cursor" --no-pager > "$out/$stamp-pos$pos.log"
	# focus_metric prints "frame meanY sharpness noise"; average frames 30-39.
	python3 -I "$project_dir/tools/focus_metric.py" "$cap" \
		| awk -v p="$pos" '$1 ~ /^[0-9]+$/ && $1 >= 30 && $1 <= 39 { s += $3; y += $2; n++ }
			END { if (n) printf "position %4d: sharpness %.3f  meanY %.1f  (%d frames)\n", p, s / n, y / n, n;
			      else printf "position %4d: no frames 30-39\n", p }' | tee -a "$summary"
done

sudo rmmod amd_isp4_capture || true
sudo modprobe amd_isp4_capture || echo "note: could not reload the installed module (reboot restores it)"
echo "summary: $summary"
