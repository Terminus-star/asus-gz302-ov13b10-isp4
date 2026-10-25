#!/usr/bin/env bash
# Offline A/B of denoise filters on one camera capture (no installs; uses the system ffmpeg).
#
#   ./scripts/denoise-ab.sh CAPTURE.nv12 [FRAME]      FRAME default 35 (after auto exposure settled)
#
# CAPTURE is a raw NV12 1920x1080 file as written by test-isp4-threshold.sh / focus-sweep.sh. Every filter chain
# processes the whole sequence (the temporal filters need the earlier frames), then FRAME is saved twice:
# NAME-full.png (whole frame) and NAME-crop.png (centre 640x360 scaled 2x with nearest neighbour, to see noise).
# Output: scratch/denoise-research/ab/ (overwritten). Prints the time each chain took.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cap=${1:?usage: $0 CAPTURE.nv12 [FRAME]}
frame=${2:-35}
out="$project_dir/scratch/denoise-research/ab"
mkdir -p "$out"
in=(-f rawvideo -pixel_format nv12 -video_size 1920x1080 -framerate 30 -i "$cap")
pick="select=eq(n\\,$frame)"
crop="crop=640:360:640:360,scale=1280:720:flags=neighbor"

# NAME|FILTERS (Vulkan chains upload to the GPU and download again; -init_hw_device is added for them)
chains=(
	"0-raw|null"
	"1-hqdn3d|hqdn3d=4:3:6:4.5"
	"2-hqdn3d-strong|hqdn3d=6:4.5:9:7"
	"3-atadenoise|atadenoise=s=7"
	"4-nlmeans-vulkan|hwupload,nlmeans_vulkan=s=3:p=3:r=7,hwdownload,format=nv12"
	"5-hqdn3d+nlmeans-vulkan|hqdn3d=3:2:6:4.5,format=nv12,hwupload,nlmeans_vulkan=s=2.5:p=3:r=7,hwdownload,format=nv12"
)

for c in "${chains[@]}"; do
	name=${c%%|*}
	vf=${c#*|}
	hw=()
	[[ $vf == *vulkan* ]] && hw=(-init_hw_device vulkan=vk -filter_hw_device vk)
	start=$(date +%s%N)	# nanoseconds; bash integer arithmetic below (no bc needed)
	ffmpeg -hide_banner -loglevel error -y "${hw[@]}" "${in[@]}" \
		-filter_complex "[0:v]$vf,$pick,split[a][b];[b]$crop[c]" \
		-map "[a]" -frames:v 1 "$out/$name-full.png" -map "[c]" -frames:v 1 "$out/$name-crop.png"
	printf '%-26s %5d ms for %s frames\n' "$name" "$((($(date +%s%N) - start) / 1000000))" "$((frame + 1))"
done
echo "images in $out"
