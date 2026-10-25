#!/usr/bin/env bash
# Test aur/gz302-camera-firmware/PKGBUILD locally, before any release exists.
#
#   ./scripts/test-aur-firmware.sh
#
# 1. Packs the build tools from this checkout into the tarball the PKGBUILD would download from GitHub
#    (same name, top folder <repo>-VERSION/), so makepkg uses it instead of downloading.
# 2. Case "no files": HOME points at an empty folder -> makepkg must refuse.
# 3. Case "files": HOME points at a folder with Cam-Firm-GZ302EA/ holding the five Windows files under
#    oddly-cased names (tests the case-insensitive search) -> makepkg must build the package.
# 4. Every file in the package is compared with the file the working camera uses today.
# Everything happens in scratch/aur-test/firmware/ (overwritten each run); the real ~ is not touched.
# makepkg needs its PKGBUILD folder as current directory, hence the cd in subshells.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
work="$project_dir/scratch/aur-test/firmware"
win=/mnt/windows/Windows/System32/DriverStore/FileRepository
ver=$(sed -n 's/^pkgver=//p' "$project_dir/aur/gz302-camera-firmware/PKGBUILD")

mkdir -p "$work/build" "$work/home-empty" "$work/home-files/Cam-Firm-GZ302EA" "$work/extract"
cp "$project_dir/aur/gz302-camera-firmware/PKGBUILD" "$work/build/"
tar -czf "$work/build/gz302-camera-firmware-$ver.tar.gz" -C "$project_dir" \
	--transform "s|^|$(basename "$project_dir")-$ver/|" \
	tools/build_winfw.py tools/pe_emu.py tools/gen_script.py tools/gen_phy_seq.py tools/gen_sensor_prop.py

echo "=== case 1: no ~/cam-firm-gz302ea (must refuse)"
if (cd "$work/build" && HOME="$work/home-empty" makepkg -f --noconfirm); then
	echo "FAIL: makepkg built without the Windows files"; exit 1
fi
echo "OK: refused"

echo "=== case 2: ~/Cam-Firm-GZ302EA with oddly-cased file names (must build)"
cp "$win/sensorov13b.inf_amd64_817a026ab537dd2a/OV13B_3001_ROG_SRX.atb" "$work/home-files/Cam-Firm-GZ302EA/ov13b_3001_rog_srx.ATB"
cp "$win/sensorov13b.inf_amd64_817a026ab537dd2a/asm_OV13B.asm" "$work/home-files/Cam-Firm-GZ302EA/ASM_OV13B.ASM"
cp "$win/amdcamera.inf_amd64_636e6e00d737b319/srx_h_isp_firmware_signed.ram" "$work/home-files/Cam-Firm-GZ302EA/SRX_H_ISP_Firmware_Signed.ram"
cp "$win/amdcamera.inf_amd64_636e6e00d737b319/amdcamera.sys" "$work/home-files/Cam-Firm-GZ302EA/AMDCamera.SYS"
cp "$win/sensorov13b.inf_amd64_817a026ab537dd2a/sensorOV13B.sys" "$work/home-files/Cam-Firm-GZ302EA/SENSOROV13B.sys"
(cd "$work/build" && HOME="$work/home-files" makepkg -f --noconfirm)

pkg=$(cd "$work/build" && HOME="$work/home-files" makepkg --packagelist)
bsdtar -xf "$pkg" -C "$work/extract"
echo "=== package contents vs the files the camera uses today"
fail=0
check() {	# check PACKAGED_PATH REFERENCE_FILE
	if cmp -s "$work/extract/$1" "$2"; then echo "identical: $1"; else echo "DIFFERS:   $1  (vs $2)"; fail=1; fi
}
check usr/lib/firmware/updates/amdgpu/isp_4_1_1.bin "$project_dir/firmware/isp_4_1_1_srx_h.bin"
check usr/lib/firmware/amdgpu/gz302-ov13b-script.bin /lib/firmware/amdgpu/gz302-ov13b-script.bin
check usr/lib/firmware/amdgpu/gz302-ov13b-phy-seq.bin /lib/firmware/amdgpu/gz302-ov13b-phy-seq.bin
check usr/lib/firmware/amdgpu/gz302-ov13b-sensor-prop.bin /lib/firmware/amdgpu/gz302-ov13b-sensor-prop.bin
check usr/lib/firmware/amdgpu/OV13B_3001_ROG_SRX.atb /lib/firmware/amdgpu/OV13B_3001_ROG_SRX.atb
exit $fail
