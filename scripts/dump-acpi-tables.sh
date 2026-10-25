#!/usr/bin/env bash
# Dump this machine's ACPI definition tables (DSDT and all SSDTs) into scratch/acpi/ and decompile
# them with iasl, so the power/reset/GPIO wiring of the camera sensor (ACPI device \_SB.CAMB, HID
# OMNI13B1) can be read.  Findings section 36.
#
# Why: the raw I2C probe (patch 0019) shows the sensor at 21-0010 never acknowledges (-EREMOTEIO), so it
# is not powered/enabled; neither the OV13B kernel driver (dummy regulators, optional reset GPIO) nor
# amd_isp4.c (only describes OMNI5C10) controls the power, and the Windows bridge driver is a stub.
# The power sequence has to be in ACPI (_PS0/_ON methods, GpioIo resources, power resources).
#
# What it does (reads sysfs, writes only inside the project):
#   1. refuses to run if scratch/acpi already exists
#   2. sudo cp  /sys/firmware/acpi/tables/{DSDT,SSDT*}  -> scratch/acpi/*.dat   (sysfs tables are root-only)
#   3. sudo chown the copies to your user
#   4. iasl -d: decompile the DSDT with all SSDTs as external references, then each SSDT -> scratch/acpi/*.dsl
#
# Run from the project directory, as your normal user:
#     ./scripts/dump-acpi-tables.sh
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
out="$project_dir/scratch/acpi"
src=/sys/firmware/acpi/tables

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[[ $EUID -ne 0 ]] || die "run as your normal user, not root (the script calls sudo itself)"
command -v iasl >/dev/null || die "iasl not found (package acpica)"
[[ ! -e $out ]] || die "$out already exists; refusing to reuse it (move it away or choose to inspect it directly)"

mkdir "$out"

# Copy as root, because the files in sysfs are 0400 root.  nullglob is not needed: DSDT exists.
for t in DSDT $(sudo ls "$src" | grep -E '^SSDT[0-9]*$'); do
	sudo cp "$src/$t" "$out/$t.dat"
done
sudo chown -R "$(id -u):$(id -g)" "$out"

ssdts=()
for f in "$out"/SSDT*.dat; do
	[[ -e $f ]] && ssdts+=("$f")
done

# DSDT first, giving the SSDTs as external symbol sources (-e), then every SSDT on its own.
iasl -p "$out/DSDT" -e "${ssdts[@]}" -d "$out/DSDT.dat" >"$out/iasl-DSDT.log" 2>&1 || true
for f in "${ssdts[@]}"; do
	b=$(basename "$f" .dat)
	iasl -p "$out/$b" -d "$f" >"$out/iasl-$b.log" 2>&1 || true
done

printf 'Done. Files in %s:\n' "$out"
ls -la "$out"
printf '\nSensor device:\n'
grep -l -E 'OMNI13B1|CAMB' "$out"/*.dsl 2>/dev/null || echo "(no .dsl mentions OMNI13B1/CAMB; check the iasl-*.log files)"
