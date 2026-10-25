#!/usr/bin/env bash
# Refresh (rebase) one patch onto a kernel tree, like "quilt refresh": apply it with fuzz allowed to a
# copy of the files it touches, then regenerate its diff from before/after, so it applies with NO
# fuzz to that tree. The change itself stays the same; only hunk positions and context lines change.
#
#   ./scripts/refresh-patch.sh PATCH TREE [OUT]
#
#   PATCH  the patch to refresh (header/commit message up to the first "---" line is kept)
#   TREE   a kernel source tree in the state BEFORE this patch (earlier patches of the series that
#          touch the same files must already be applied there)
#   OUT    where to write the refreshed patch (default: overwrite PATCH)
#
# Work files: scratch/refresh/NAME/{a,b}/ (overwritten each run).
# ALWAYS read the fuzz lines it prints: a fuzzed hunk is the one place a change can land wrongly.
set -euo pipefail

project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
patch_in=${1:?usage: $0 PATCH TREE [OUT]}
tree=${2:?usage: $0 PATCH TREE [OUT]}
out=${3:-$patch_in}
work="$project_dir/scratch/refresh/$(basename "$patch_in" .patch)"

mapfile -t files < <(sed -n 's|^+++ b/||p' "$patch_in")
for f in "${files[@]}"; do
	mkdir -p "$work/a/$(dirname "$f")" "$work/b/$(dirname "$f")"
	cp "$tree/$f" "$work/a/$f"
	cp "$tree/$f" "$work/b/$f"
done

# Apply with up to 2 lines of fuzz; print offsets/fuzz so they can be checked by hand.
patch -p1 -F2 --no-backup-if-mismatch -d "$work/b" < "$patch_in" | grep -i "fuzz\|offset\|FAILED" || true

new="$work/refreshed.patch"	# overwritten each run
sed -n '1,/^---$/p' "$patch_in" > "$new"
for f in "${files[@]}"; do
	echo "diff --git a/$f b/$f" >> "$new"
	diff -u --label "a/$f" --label "b/$f" "$work/a/$f" "$work/b/$f" >> "$new" || [[ $? -eq 1 ]]
done
cp "$new" "$out"
echo "wrote $out"
