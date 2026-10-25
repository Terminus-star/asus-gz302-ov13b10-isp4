# Shared settings for building out-of-tree modules; SOURCED by the build scripts, not run on its own.
#
# Sets, for the kernel to build against:
#   KVER        kernel release (default: the running kernel, uname -r; or set KVER=... before running)
#   KDIR        its build directory (default: /usr/lib/modules/$KVER/build, installed by the -headers
#               package; or set KDIR=... e.g. for headers extracted into scratch/)
#   KMAKE_CC    extra make arguments for the compiler that built that kernel: (LLVM=1) for clang,
#               () for GCC. Read from the kernel's own .config, because a module must be built with
#               the same compiler family as its kernel.

# Default: the RUNNING kernel, because insmod only loads a module built for the running kernel. The camera works
# on the stock linux-cachyos kernel with gz302-camera-dkms installed (ACPI camera table; findings 136-138).
KVER=${KVER:-$(uname -r)}
KDIR=${KDIR:-/usr/lib/modules/$KVER/build}

if [[ ! -f $KDIR/.config ]]; then
	echo "ERROR: no kernel headers for $KVER at $KDIR (install the matching -headers package)" >&2
	exit 1
fi
if grep -q '^CONFIG_CC_IS_CLANG=y' "$KDIR/.config"; then
	KMAKE_CC=(LLVM=1)
else
	KMAKE_CC=()
fi
