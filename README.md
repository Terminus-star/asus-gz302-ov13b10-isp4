# Rear camera for the ASUS ROG Flow Z13 2025 (GZ302EA) on Linux

The rear camera of the 2025 Flow Z13 (AMD Ryzen AI Max "Strix Halo") does not work on stock Linux: the
OmniVision **OV13B10** sensor sits behind AMD's image signal processor (**ISP 4.1**), and the kernel's ISP
drivers only know AMD's reference board. This repository makes it work on a **stock Arch or CachyOS kernel**
(officially tested: `linux-cachyos` 7.2+): a DKMS package with the camera drivers and a small ACPI table, a
firmware package that you build from your own Windows install, and the notes and tools from the bring-up. No
kernel has to be compiled.

> **Status: working, still being tuned.** `/dev/video0` gives live **1920x1080 NV12 at 30 fps**
> (YUYV and 1920x1440 also offered) with auto exposure and neutral colours; tested with ffplay and OBS.
> **Manual focus** works as the standard control `focus_absolute` (e.g. the focus slider in OBS).
> The module's factory calibration (lens shading, white balance, AF data) is loaded like on Windows.
> Not done yet: autofocus (an experimental driver-side autofocus exists, off by default), white balance
> and image tuning matched to Windows. The front and IR cameras are separate USB cameras and already work on
> stock Linux.

## Install (Arch / CachyOS)

**Before you start:**

- A kernel from **6.18** on with its **headers package** installed (DKMS builds the drivers against it; since
  `gz302-camera-dkms` 0.4.0 no headers package is forced on you). DKMS builds for every installed kernel that has
  its headers:

  | kernel | headers package | status |
  |---|---|---|
  | `linux-cachyos` 7.2+ | `linux-cachyos-headers` | **officially tested** |
  | `linux-cachyos-lts`, `-rc`, `-deckify` | `linux-cachyos-lts-headers`, ... | builds; up to you to test |
  | Arch `linux`, `linux-lts`, `linux-zen` | `linux-headers`, `linux-lts-headers`, `linux-zen-headers` | builds; up to you to test |

  "Builds" means the drivers compile cleanly against that kernel's headers (findings 143); whether the camera
  works there has not been checked. Reports are welcome (see below).
- **mkinitcpio** for the initramfs (the Arch and CachyOS default); the package adds an ACPI table to it.
- Your **Windows partition, mounted** (read-only is enough; unlock it first if it uses BitLocker), e.g. at
  `/mnt/windows`. Five camera files are taken from it (see below); nothing from Windows is distributed here.
- **`python-unicorn`** (`sudo pacman -S python-unicorn`): the firmware build runs two functions of your Windows
  driver files in this CPU emulator (see "What it takes"); the installer checks for it.
- Tested on **CachyOS with the Limine bootloader**.

1. Clone this repository and run the installer (the packages are not on the AUR yet, because the AUR is not
   accepting new accounts at the moment):

   ```sh
   git clone https://github.com/Terminus-star/asus-gz302-ov13b10-isp4.git
   cd asus-gz302-ov13b10-isp4
   bash scripts/install-local-packages.sh
   ```

   The script copies the five Windows files into `~/cam-firm-gz302ea/` from the mounted Windows partition
   ([`scripts/copy-windows-files.sh`](scripts/copy-windows-files.sh); it mounts nothing and keeps files already
   there), checks the kernel headers and that no camera files copied by hand are in the way (it lists every
   problem with the command to fix it), then builds and installs
   `gz302-camera-firmware` and `gz302-camera-dkms`. It asks for sudo before installing anything and stops at the
   first problem. Running it again is safe: it skips packages already installed at that version
   (`--rebuild` builds them again, `--dry-run` only shows what it would do). Installing the packages rebuilds the
   initramfs of your kernels automatically. They are normal pacman packages: `pacman -Q gz302-camera-dkms` shows
   them, `pacman -R gz302-camera-dkms gz302-camera-firmware` removes everything again (drivers, ACPI table,
   firmware files) and rebuilds the initramfs without them.

   Without a mounted Windows partition, copy the files into `~/cam-firm-gz302ea/` (any upper/lower case) yourself,
   e.g. from a USB stick filled while Windows runs:

   | file | Windows folder |
   |---|---|
   | `OV13B_3001_ROG_SRX.atb` | `C:\Windows\System32\DriverStore\FileRepository\sensorov13b.inf_amd64_*\` |
   | `asm_OV13B.asm` | `C:\Windows\System32\DriverStore\FileRepository\sensorov13b.inf_amd64_*\` |
   | `sensorOV13B.sys` | `C:\Windows\System32\DriverStore\FileRepository\sensorov13b.inf_amd64_*\` |
   | `srx_h_isp_firmware_signed.ram` | `C:\Windows\System32\DriverStore\FileRepository\amdcamera.inf_amd64_*\` |
   | `amdcamera.sys` | `C:\Windows\System32\DriverStore\FileRepository\amdcamera.inf_amd64_*\` |

   The same install by hand, in this order (the DKMS package depends on the firmware package):

   ```sh
   cd aur/gz302-camera-firmware && makepkg -si
   cd ../gz302-camera-dkms && makepkg -si
   ```

2. Reboot and check:

   ```sh
   cat /sys/bus/acpi/devices/OMNI13B1:00/modalias        # ends in :OMNI5C10:
   modinfo -n amd_isp4_capture ov13b10 amd_isp4          # paths under updates/dkms/
   ffplay -f v4l2 -video_size 1920x1080 -input_format nv12 /dev/video0
   ```

3. Please report back, working or not: `bash scripts/report.sh --check` writes a short report (versions, DKMS
   status, the camera's kernel log lines and a 3-second image check; no hostname, serials or Windows files) to
   `scratch/reports/`. Attach it to an issue. Reports from more GZ302 owners are what upstream support needs.

**Updating:** `git pull` in the clone and run the script again. A new kernel needs nothing from you: DKMS
builds the drivers for it when its headers are installed. If that build fails (a kernel whose ISP
driver changed), please open an issue with the DKMS build log (`/var/lib/dkms/gz302-camera/*/build/make.log`).

Details and troubleshooting: [`aur/README.md`](aur/README.md).

## Fedora / Ubuntu (TODO)

Not supported yet: the installer needs pacman and makepkg. Planned: an installer without pacman that builds the
firmware with the same tools, registers the drivers with `dkms add/build/install`, and adds the ACPI table
through dracut (Fedora, `acpi_override="yes"`) or an early initrd (Ubuntu, initramfs-tools has no ACPI table
option). Points already known:

- Fedora and Ubuntu reportedly put the kernel in lockdown when Secure Boot is on (not verified here; Arch and
  CachyOS kernels do not: Secure Boot on, lockdown `[none]`, camera works), and a locked-down kernel ignores
  ACPI tables from the initramfs ("kernel is locked down, ignoring table override"), so the camera table would
  not load there; this needs a solution or a documented requirement.
- Ubuntu 26.04's dkms (3.2.2) predates `BUILD_EXCLUSIVE_KERNEL_MIN` (dkms 3.3.0), so the installer has to check
  the kernel version itself.
- `tools/build_winfw.py` reads the stock ISP firmware raw or as `.zst`; other compressions (`.xz`) still need
  adding.

## What it takes

The camera needs changes in three drivers, one ACPI table, and a set of firmware and data files.

```
amdgpu (ISP power, firmware)         unchanged: the ACPI table makes it find the GZ302's camera
  └─ amd_isp4 (board description)    0002 describe the OV13B10 and its CSI-2 link, 0008 sensor reset GPIO
       ├─ ov13b10 (sensor driver)    0004 I2C id, 0006 GZ302 PLL and mode registers
       └─ amd_isp4_capture (V4L2)    0007 everything the ISP firmware needs to stream (from 0009-0055)
```

| change | where | why |
|---|---|---|
| ACPI table ([`acpi/`](acpi/)) | initramfs | amdgpu only starts the ISP for AMD's reference sensor (`OMNI5C10`); the GZ302 reports `OMNI13B1`. The table adds `OMNI5C10` as a *compatible* ID to the camera, so the stock amdgpu starts the ISP (findings 136) |
| 0002 | amd_isp4 | ASUS's ACPI table describes no I2C address, lanes or clock for the sensor; this adds them |
| 0004 | ov13b10 | lets the sensor driver bind to the I2C device the board code creates |
| 0006 | ov13b10 | the PLL and mode registers Windows uses on this laptop (wrong values give a magenta image) |
| 0007 | amd_isp4_capture | firmware memory pool sized like Windows, MMHUB prefetch, I2C descriptors in the device script, per-frame meta-data buffers, sensor start, data files loaded as firmware, manual focus through the firmware's I2C, module calibration, the `enable_isp` GPIO on the GZ302 |
| 0008 | amd_isp4 | maps the sensor reset line so the sensor powers up before its I2C probe |

The patches are in [`dkms/patches/`](dkms/patches/); `gz302-camera-dkms` is generated from them and the
CachyOS kernel sources by [`scripts/make-aur-dkms.sh`](scripts/make-aur-dkms.sh).

Run-time files (in `/usr/lib/firmware/`, installed by `gz302-camera-firmware`):

| file | made from | shipped here? |
|---|---|---|
| `updates/amdgpu/isp_4_1_1.bin` | the Windows ISP firmware (`srx_h_isp_firmware_signed.ram`) with the Linux header | no, built on your machine |
| `amdgpu/gz302-ov13b-script.bin` | the Windows sensor script `asm_OV13B.asm`, assembled | no, built on your machine |
| `amdgpu/OV13B_3001_ROG_SRX.atb` | the Windows sensor tuning file, unchanged | no, your copy |
| `amdgpu/gz302-ov13b-phy-seq.bin` | the MIPI D-PHY receiver register sequence, from `amdcamera.sys` | no, built on your machine |
| `amdgpu/gz302-ov13b-sensor-prop.bin` | the sensor property block, from `sensorOV13B.sys` | no, built on your machine |

AMD's and ASUS's files are never distributed here, and neither is any file derived from them (the driver patches
only carry a few OV13B10 register values, the sensor settings the GZ302 needs): the firmware package builds every
file above from the copies you provide, with [`tools/build_winfw.py`](tools/build_winfw.py),
and [`tools/gen_script.py`](tools/gen_script.py) / [`tools/gen_phy_seq.py`](tools/gen_phy_seq.py) /
[`tools/gen_sensor_prop.py`](tools/gen_sensor_prop.py), which run one function of your own driver file in a CPU
emulator ([`tools/pe_emu.py`](tools/pe_emu.py), python-unicorn) and record its result; nothing touches hardware.

## Repository layout

| path | contents |
|---|---|
| [`aur/`](aur/) | the two packages (`gz302-camera-dkms`, `gz302-camera-firmware`) and their instructions |
| [`dkms/patches/`](dkms/patches/) | the patches the DKMS package is built from (0002, 0004, 0006, 0007, 0008) |
| [`acpi/`](acpi/) | the ACPI table source and its mkinitcpio drop-in (shipped by the DKMS package) |
| [`driver/`](driver/) | the capture driver as an out-of-tree module, with its development history as patches 0009-0055 ([`driver/README.md`](driver/README.md)) |
| [`tools/`](tools/) | the firmware package's build tools (firmware image, sensor script, the two emulated data files), image metrics |
| [`scripts/`](scripts/) | build, test and release scripts (each one starts with a comment saying what it does) |
| [`docs/`](docs/) | [`bringup-summary.md`](docs/bringup-summary.md): what Linux needed, results, open questions |

The repository was renamed from `camera-bringup` and its history was restarted on 2026-10-25. If you cloned it
before, delete your clone and clone it again (installed packages are not affected).

## How it was done

The Linux ISP 4.1 driver works on AMD's reference board but not here, and the working Windows setup on the same
laptop was the only reference. The bring-up compared the commands each side sends to the ISP firmware and made many
test runs on Linux, each changing one thing. What Linux needed and what is still open is summarised in
[`docs/bringup-summary.md`](docs/bringup-summary.md).

## Upstream

The kernel side needs no custom kernel any more, but it is not upstream. AMD maintains the ISP4 drivers; on the
mailing list AMD noted that this sensor (`OMNI13B1`) needs a new amdgpu entry with its GPIO tables, and that new
sensors need both ISP firmware and kernel updates. The capture-driver patch 0007 still contains experiment code and
needs cleaning up and splitting, and the ISP firmware that supports this sensor would have to come from AMD through
linux-firmware.

## Contributing

Reports from other GZ302 owners are welcome, especially whether the installer says "different version" for your
Windows files and whether the camera still works then. Please attach the file from `bash scripts/report.sh --check`.
For debugging, `sudo modprobe -r amd_isp4_capture && sudo modprobe amd_isp4_capture exp_verbose=1` brings back the
driver's detailed trace (`EXP-...` lines) in the kernel log.

## License and disclaimer

GPL-2.0 ([`LICENSE`](LICENSE)), the license of the kernel code the patches change. Not affiliated with
ASUS, AMD or OmniVision. The files you copy from Windows remain their owners' property. The DKMS package
replaces three of your kernel's drivers and adds an ACPI table to your initramfs; `pacman -R gz302-camera-dkms`
undoes both.

Built on the upstream AMD ISP4 drivers and CachyOS's kernel sources.
