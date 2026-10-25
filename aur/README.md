# GZ302 rear camera packages (AUR)

Two Arch/CachyOS packages that make the rear camera (OmniVision OV13B10 behind the AMD ISP 4.1) of the
**ASUS ROG Flow Z13 2025 (GZ302EA)** work on a stock kernel (officially tested: `linux-cachyos` 7.2+):

| package | what it is |
|---|---|
| `gz302-camera-firmware` | firmware and data files for the camera, built on your machine from five files you copy from your own Windows install |
| `gz302-camera-dkms` | the camera drivers (`amd_isp4_capture`, `ov13b10`, `amd_isp4`) with [`dkms/patches/`](../dkms/patches/) 0002, 0004, 0006, 0007, 0008, as source; DKMS builds them for each installed kernel from 6.18 on that has its headers, and they replace the kernel's own copies. Also installs a small ACPI table ([`acpi/`](../acpi/), adds the compatible ID `OMNI5C10` to the camera) and a mkinitcpio drop-in, so the stock kernel's amdgpu finds the camera; every initramfs is rebuilt on install and removal (findings 136-138) |

`gz302-camera-dkms` depends on `gz302-camera-firmware` (and `dkms`); install the headers package of each kernel
you boot (`linux-cachyos-headers` for the tested kernel, an optional dependency). Driver updates are new
versions of `gz302-camera-dkms`. The camera is still being tuned: autofocus is experimental and off (manual focus
works, `focus_absolute`), and colours are not yet matched to Windows. The driver's detailed trace (`EXP-...` lines)
is debug output since 0.3.0; module parameter `exp_verbose=1` (or `dyndbg=+p`) shows it.

## Before you start

- A kernel from 6.18 on and its headers package (e.g. `linux-cachyos-headers`). Officially tested: `linux-cachyos`
  7.2+. Other kernels (CachyOS `-lts`, `-rc`, `-deckify`; Arch's `linux`, `linux-lts`, `linux-zen`, ...) build
  (findings 143) but are up to their users to test; DKMS skips kernels older than 6.18.
- mkinitcpio for the initramfs (the CachyOS default). Tested on CachyOS with Limine.
- `python-unicorn` for building the firmware package (it runs two functions of your Windows driver files in this
  CPU emulator to produce two of its data files).

## 1. Copy five files from Windows

With a clone of the repository and the Windows partition mounted (read-only is enough), `bash
scripts/copy-windows-files.sh` does this step (the installer in step 2 runs it too). By hand: create the folder
`~/cam-firm-gz302ea/` (upper/lower case does not matter) and copy into it:

| file | from (in Windows) |
|---|---|
| `OV13B_3001_ROG_SRX.atb` | `C:\Windows\System32\DriverStore\FileRepository\sensorov13b.inf_amd64_*\` |
| `asm_OV13B.asm` | `C:\Windows\System32\DriverStore\FileRepository\sensorov13b.inf_amd64_*\` |
| `sensorOV13B.sys` | `C:\Windows\System32\DriverStore\FileRepository\sensorov13b.inf_amd64_*\` |
| `srx_h_isp_firmware_signed.ram` | `C:\Windows\System32\DriverStore\FileRepository\amdcamera.inf_amd64_*\` |
| `amdcamera.sys` | `C:\Windows\System32\DriverStore\FileRepository\amdcamera.inf_amd64_*\` |

From Linux you can read them from the Windows partition (mount it read-only), or copy them to a USB
stick while Windows is running. They are AMD/ASUS files and are not distributed by these packages: the
firmware package uses your copies to build the ISP firmware image, the sensor script, the D-PHY sequence and the
sensor properties locally (the last two by running one function of each `.sys` file in a CPU emulator).

## 2. Install

The packages are not on the AUR yet (the AUR is not accepting new accounts at the moment). Until then,
build them from a clone of the repository:

```sh
git clone https://github.com/Terminus-star/asus-gz302-ov13b10-isp4.git
cd asus-gz302-ov13b10-isp4
bash scripts/install-local-packages.sh
```

The script checks the prerequisites first (the five files, copied from a mounted Windows partition if missing;
python-unicorn; the kernel headers; no hand-copied camera files in the way), then builds and installs `gz302-camera-firmware` and `gz302-camera-dkms`, asking for sudo before each
install. By hand, in this order:

```sh
cd aur/gz302-camera-firmware && makepkg -si
cd ../gz302-camera-dkms && makepkg -si
```

Once they are on the AUR: `paru -S gz302-camera-dkms` (builds the firmware package first).

If you copied camera files into `/lib/firmware/amdgpu/` by hand before, pacman stops with
"exists in filesystem": remove those files (`gz302-ov13b-*.bin`, `OV13B_3001_ROG_SRX.atb`) and install again.

## 3. Boot and test

Reboot (the ACPI table and the ISP firmware are loaded at boot). Then:

```sh
cat /sys/bus/acpi/devices/OMNI13B1:00/modalias        # ends in :OMNI5C10: (the ACPI table is active)
dkms status gz302-camera                              # installed for your kernel
modinfo -n amd_isp4_capture ov13b10 amd_isp4          # paths under updates/dkms/
ls /dev/video*
ffplay /dev/video0                                    # or OBS, a browser, ...
```

If the modalias has no `:OMNI5C10:`, the initramfs was not rebuilt with the table: run `sudo limine-mkinitcpio`
(or `sudo mkinitcpio -P`) and reboot. Otherwise run `bash scripts/report.sh` and attach its file to an issue.

## Notes

- The ISP firmware image is installed to `/usr/lib/firmware/updates/amdgpu/isp_4_1_1.bin`, which the
  kernel and mkinitcpio prefer over the linux-firmware image, so other kernels load it too
  (without the camera drivers nothing else changes for them).
- The ACPI table is in every kernel's initramfs. On kernels DKMS skips (older than 6.18, or without headers),
  amdgpu then also starts the ISP, and the kernel's own unpatched drivers find no camera they know; untested beyond that.
- New kernels need nothing: DKMS builds the drivers when their headers are installed. If the build fails
  (a kernel whose ISP4 driver changed), see "New kernel breaks the DKMS build" below.

## Maintainers: version bump and release

From the repository root:

1. Driver change: new `driver/patches/00NN`, then
   `bash scripts/update-capture-patch.sh kernel/build/tarballs/cachyos-X.Y.Z-N.tar.gz NN NN`, raise `dkms_ver` in
   `scripts/make-aur-dkms.sh`, `bash scripts/make-aur-dkms.sh kernel/build/tarballs/cachyos-X.Y.Z-N.tar.gz`, then
   `bash scripts/install-local-packages.sh`.
2. New kernel breaks the DKMS build: download that kernel's CachyOS source tarball and its signature into
   `kernel/build/tarballs/` (`https://github.com/CachyOS/linux/releases/download/cachyos-X.Y.Z-N/cachyos-X.Y.Z-N.tar.gz`
   and `.asc`), check it with `gpg --verify` (CachyOS's keys: `gpg --recv-keys E8B9AA39F054E30E8290D492C3C4820857F654FE
   E18447AC260021D31F3FF6C4C8A2A4774B8B63C4`), rebase the failing patch with `scripts/refresh-patch.sh`, raise
   `dkms_ver` and run `scripts/make-aur-dkms.sh` with the new tarball.
3. New firmware package version: change `pkgver`, set `b2sums=('SKIP')`, tag `v<pkgver>` and push the
   tag, then `bash scripts/aur-firmware-checksum.sh`.
4. `bash scripts/aur-sync.sh` copies both packages into `aur-git/` and prints the commit/push commands.
