#!/usr/bin/env bash
# Fetch the kernel source files the GZ302 camera patches touch, for one kernel tag, from the GitHub mirrors.
#
#   ./scripts/fetch-upstream-src.sh REPO TAG
#   e.g. ./scripts/fetch-upstream-src.sh torvalds/linux v7.3-rc6     (mainline)
#        ./scripts/fetch-upstream-src.sh gregkh/linux   v7.2.9       (stable)
#
# Writes only into scratch/upstream-src/TAG/ (a new folder; refuses to run if it exists), keeping the kernel paths.
# Downloaded files are data: nothing is executed or built here.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
repo=${1:?usage: $0 REPO TAG}
tag=${2:?usage: $0 REPO TAG}
out="$project_dir/scratch/upstream-src/$tag"

[[ -e $out ]] && { echo "ERROR: $out exists, not overwriting" >&2; exit 1; }

files=(
	Makefile
	drivers/media/platform/amd/isp4/Kconfig
	drivers/media/platform/amd/isp4/Makefile
	drivers/media/platform/amd/isp4/isp4.c
	drivers/media/platform/amd/isp4/isp4.h
	drivers/media/platform/amd/isp4/isp4_debug.c
	drivers/media/platform/amd/isp4/isp4_debug.h
	drivers/media/platform/amd/isp4/isp4_fw_cmd_resp.h
	drivers/media/platform/amd/isp4/isp4_hw_reg.h
	drivers/media/platform/amd/isp4/isp4_interface.c
	drivers/media/platform/amd/isp4/isp4_interface.h
	drivers/media/platform/amd/isp4/isp4_subdev.c
	drivers/media/platform/amd/isp4/isp4_subdev.h
	drivers/media/platform/amd/isp4/isp4_video.c
	drivers/media/platform/amd/isp4/isp4_video.h
	drivers/gpu/drm/amd/amdgpu/amdgpu_acpi.c
	drivers/gpu/drm/amd/amdgpu/isp_v4_1_1.c
	drivers/platform/x86/amd/amd_isp4.c
	drivers/media/i2c/ov13b10.c
)

for f in "${files[@]}"; do
	mkdir -p "$out/$(dirname "$f")"
	code=$(curl -s -w '%{http_code}' -o "$out/$f" "https://raw.githubusercontent.com/$repo/$tag/$f")
	if [[ $code != 200 ]]; then
		echo "missing ($code): $f"
		rm -f "$out/$f"	# only the failed download this script just created
	else
		echo "ok: $f"
	fi
done
