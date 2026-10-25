#!/usr/bin/env bash
# Regression check of the rear camera AS INSTALLED (no module reload, no sudo): run it once on each kernel with the
# same scene and lens position, then compare the two result lines.
#
#   ./scripts/camera-check.sh [FOCUS]      FOCUS = focus_absolute 0..1023, default 340 (BenQ logo at ~110 mm)
#
# 1. checks: the ISP video node exists, NV12 1920x1080 is offered, the focus_absolute control is present
# 2. sets focus_absolute (written to the motor at the next stream start), captures 90 frames (3 s) with v4l2-ctl
#    and keeps its fps readings
# 3. tools/focus_metric.py over the frames: mean brightness, sharpness and frame-to-frame noise of frames 60-89
#    (auto exposure has settled by then)
# 4. camera errors in the kernel log during the capture (journalctl -k, readable as your user)
# Output: scratch/camera-check/STAMP-KERNEL.{nv12,txt}; one summary line is appended to scratch/camera-check/results.txt.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
focus=${1:-340}
out="$project_dir/scratch/camera-check"
kver=$(uname -r)
stamp=$(date +%Y%m%d-%H%M%S)
base="$out/$stamp-$kver"
die() { echo "ERROR: $*" >&2; exit 1; }
mkdir -p "$out"

# ---- 1. checks ----
node=""
for n in /sys/class/video4linux/video*; do
	name=$(<"$n/name")
	path=$(readlink -f "$n/device" 2>/dev/null || true)
	if [[ ${name,,} == *isp* || ${path,,} == *isp* ]]; then node="/dev/${n##*/}"; break; fi
done
[[ -n $node ]] || die "no ISP video node: the camera driver did not come up (see journalctl -k -b | grep -i isp)"
v4l2-ctl -d "$node" --list-formats-ext > "$base-formats.txt"
grep -q "NV12" "$base-formats.txt" || die "$node does not offer NV12"
v4l2-ctl -d "$node" --list-ctrls > "$base-ctrls.txt"
grep -q "focus_absolute" "$base-ctrls.txt" || die "$node has no focus_absolute control"
modpaths=$(modinfo -n amd_isp4_capture ov13b10 amd_isp4 | tr '\n' ' ')

# ---- 2. capture ----
cursor=$(journalctl -k -n 0 --show-cursor --no-pager | sed -n 's/^-- cursor: //p')
v4l2-ctl -d "$node" --set-ctrl focus_absolute="$focus"
timeout 15s v4l2-ctl -d "$node" --set-fmt-video=width=1920,height=1080,pixelformat=NV12 \
	--stream-mmap=4 --stream-count=90 --stream-to="$base.nv12" --stream-poll > "$base-v4l2.txt" 2>&1 || true
frames=$(( $(stat -c %s "$base.nv12") / (1920 * 1080 * 3 / 2) ))
fps=$(grep -o '[0-9.]* fps' "$base-v4l2.txt" | tail -1 | cut -d' ' -f1)
sleep 1

# ---- 3. image ----
python3 -I "$project_dir/tools/focus_metric.py" "$base.nv12" > "$base-metric.txt"
img=$(awk '$1 ~ /^[0-9]+$/ && $1 >= 60 && $1 <= 89 && $4 != "-" { y += $2; s += $3; t += $4; n++ }
	END { if (n) printf "meanY %.1f sharpness %.2f noise %.2f (%d frames)", y / n, s / n, t / n, n; else printf "no frames 60-89" }' \
	"$base-metric.txt")

# ---- 4. errors during the capture ----
journalctl -k --after-cursor "$cursor" --no-pager -o cat > "$base-kernel.txt"
errors=$(grep -v EXP-FWLOG "$base-kernel.txt" | sed 's/(0 ok, 1 fail, 2 skipped)//; s/fw_error_code 0x0000//' |
	grep -c -i -E "fail|error|timeout|bus ready|status [12] " || true)

line="$stamp $kver focus=$focus frames=$frames/90 fps=${fps:-?} $img camera-errors=$errors"
echo "$line" | tee -a "$out/results.txt"
echo "modules: $modpaths"
echo "details: $base-*.txt; all results: $out/results.txt"
