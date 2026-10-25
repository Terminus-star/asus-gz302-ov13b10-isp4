#!/usr/bin/env bash
# Build the experimental amd_isp4_capture.ko from driver/ (the capture driver with all tuning parameters).
#
# What it does: compiles driver/ as an out-of-tree module against the headers of the RUNNING kernel
# (/usr/lib/modules/$(uname -r)/build), with the compiler that kernel was built with (clang -> LLVM=1,
# GCC -> nothing; read from its .config by scripts/kbuild-env.sh). So the module's vermagic matches the
# kernel you test on. Another kernel: KVER=<release> or KDIR=<build dir> before the command.
#
# Nothing is installed or loaded.  Output: driver/amd_isp4_capture.ko
#
# Run from the project directory:   ./scripts/build-isp4-threshold.sh
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src="$project_dir/driver"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

source "$project_dir/scripts/kbuild-env.sh"	# KVER, KDIR, KMAKE_CC
[[ -f $src/Kbuild ]] || die "missing $src/Kbuild"

echo "building for $KVER (${KMAKE_CC[*]:-GCC}) with $KDIR"
# 4 jobs at low priority: the earlier full-kernel builds froze the desktop; this one is small.
nice -n 15 make -C "$KDIR" M="$src" "${KMAKE_CC[@]}" -j4 modules

ko="$src/amd_isp4_capture.ko"
[[ -f $ko ]] || die "build finished but $ko was not produced"

printf '\n--- built module ---\n'
modinfo "$ko" | grep -E '^(filename|vermagic|depends|parm):' || true
printf '\nExpected vermagic to start with: %s\n' "$KVER"
