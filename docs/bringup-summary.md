# Bring-up summary: OV13B10 rear camera on AMD ISP 4.1 (ASUS ROG Flow Z13 GZ302EA)

What the Linux side needed to get the rear camera streaming, what works today and what is still open. "Observed" means
measured on this laptop; "inference" means our explanation, not confirmed by AMD.

## Hardware

- Ryzen AI MAX+ 395 (Strix Halo), ISP 4.1.1.
- Rear camera: OmniVision OV13B10, ACPI `\_SB.CAMB`, `_HID "OMNI13B1"`, `_DDN "OV13B-RGB"`, panel BACK, module variant
  "3001". Front and IR cameras are USB (uvcvideo) and not part of this.

## Status (Linux 7.2, packages 0.4.0 / firmware 0.2.0)

- 1920x1080 NV12 (and YUYV) at 30 fps from the 4208x3120 sensor mode; 90/90 frames per test run, 0 camera errors.
- Auto exposure and white balance work; manual focus works (`focus_absolute`).
- Not done: autofocus (an experimental host-side loop exists, off by default), colours matched to Windows.

## Firmware

The stock linux-firmware `isp_4_1_1.bin` does not start this sensor (`SET_SENSOR_PROP` fails with status 1 / error
0x1b; later commands get no answer). The camera runs with the ISP firmware and sensor setup data from the owner's own
Windows driver installation, which the firmware package reads from the Windows partition and builds locally; nothing
from it is distributed here. Official firmware and data for this sensor in linux-firmware can only come from AMD.

## What Linux needed

1. **ACPI / amdgpu.** `\_SB.CAMB` has `_HID "OMNI13B1"` and no `_CID`, `_CRS`, I2C or GPIO resources; amdgpu only
   recognises `OMNI5C10`, so no ISP devices appear (observed). Local workaround: an SSDT adding `_CID "OMNI5C10"`
   ([`acpi/`](../acpi/)). The proper fix is an `OMNI13B1` entry plus GPIO tables in amdgpu.
2. **GPIOs.** `enable_isp` = AMDI0030:00 pin 85 and sensor reset = amdisp-pinctrl GPIO 0 (active low), both copied
   from the reference platform (not confirmed for this board). The ISP works with pin 85 never driven (observed). If
   the ISP is power-gated after probe, a runtime-PM cycle of the sensor before streaming is needed (observed).
3. **Firmware memory pool.** 100 MiB fails `START_STREAM` (0x27, "no buffer found"); the 4208x3120 mode needs
   0x10650000 bytes (262.31 MiB), which the driver now allocates (observed).
4. **MMHUB prefetch.** Without the global `ENABLE_PREFETCH` (0x01030003) the raw write DMA failed with
   `INTR_LATENCY_BUFFER_LEAKING` and no frames, sometimes freezing the machine; payload {1, 0, 0} (RT-pipe prefetch
   only) fixed it (observed). {1, 1, 1} hung the firmware; which switch causes it is open.
5. **Metadata buffers.** The firmware needs 6 buffers of type 13 (META_DATA) as well as META_INFO, re-sent after
   every frame; otherwise "No meta data buffer is available" about 50 times per run (observed).
6. **Sensor clock, PLL, D-PHY.** With the 19.2 MHz PLL values the receiver logged 2-bit ECC errors (482 per run) and
   no frames; PLL values giving 1120 Mbps/lane from a 24 MHz clock removed them (observed; that the clock is 24 MHz
   is inference). The D-PHY receiver is started from the host before streaming (296 register operations); whether
   that is strictly needed once the PLL is right is not isolated.
7. **Colour.** The in-tree ov13b10 mode table gave a magenta image; seven register values (0x3813 0x08 instead of
   0x09, plus 0x3600, 0x380f, 0x3011, 0x3774, 0x3823, 0x3827) give neutral colour (observed); 0x3813 (a one-row Bayer
   shift) is the likely cause, not yet isolated.
8. **I2C sharing.** Host I2C transfers during streaming broke the firmware's own sensor writes; writing through the
   firmware (`SEND_I2C_MSG`, 0x02090003) works while streaming (observed) and is used for manual focus.
9. **3A, autofocus, calibration.** `SET_3A_CONFIG` is accepted but AF stays passive; the AF command group (0x33) has
   no handler in this firmware; the firmware computes a lens target but never moves the motor. Module calibration
   (`SET_M2M_CAL_DATA`, read from the module EEPROM) is accepted and clears the calibration errors.
10. **Driver bug.** `start_streaming` in the AMD ISP4 capture driver walks `buf_list` without its lock while
    frame-done removes entries, which oopsed with long work inside the loop.

## Open questions (sent to AMD)

- Firmware and sensor data for OV13B10 / OMNI13B1 in linux-firmware.
- Confirmation of the `enable_isp` and sensor reset GPIOs.
- The intended `ENABLE_PREFETCH` setting and how the driver should derive the pool size.
- Whether a host-side D-PHY start is expected for this sensor.
- Whether the AF command group is planned for the Linux firmware, and any sensor-agnostic rework.

Reports from other GZ302 owners help: `bash scripts/report.sh --check` (see the [README](../README.md)).
