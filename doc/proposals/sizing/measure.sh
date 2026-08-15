#!/bin/sh
# Reproduce the code/RAM figures in doc/proposals/2026-08-09-large-payloads-cost.md §1.
#
# NOT a build of the project. This compiles blob_db.c and a sketch of the
# proposed segmentation layer against stub Zephyr headers, purely to size them.
# Absolute numbers are host-arch; the ratios are the result that matters.
#
# Usage:  sh doc/proposals/sizing/measure.sh  [cc]
set -e

CC=${1:-gcc}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

CFLAGS="-Os -c -std=c11 -ffunction-sections -fdata-sections
        -DCONFIG_BLOB_DB_SECTOR_BUF_SIZE=4096 -DCONFIG_BLOB_DB_LOG_LEVEL=3
        -I$HERE/stub -I$ROOT/include -I$ROOT/lib/blob_db"

sizes() {   # $1 = object file
	size -A "$1" | awk '
		/^\.text/   { t += $2 }
		/^\.rodata/ { r += $2 }
		/^\.bss/    { b += $2 }
		END { printf "text=%-6d rodata=%-6d bss=%d\n", t, r, b }'
}

echo "== baseline: lib/blob_db/blob_db.c =="
for P in 256 4066; do
	$CC $CFLAGS -DCONFIG_BLOB_DB_MAX_PAYLOAD_LEN=$P \
	    "$ROOT/lib/blob_db/blob_db.c" -o "$OUT/base_$P.o"
	printf "  MAX_PAYLOAD_LEN=%-6s " "$P"
	sizes "$OUT/base_$P.o"
done

echo "== proposed: segmentation layer (blob_db_seg.c) =="
for K in 33 131 251 512; do
	$CC $CFLAGS -DCONFIG_BLOB_DB_MAX_PAYLOAD_LEN=2026 \
	    -DCONFIG_BLOB_DB_MAX_SEGMENTS=$K \
	    "$HERE/blob_db_seg.c" -o "$OUT/seg_$K.o"
	printf "  MAX_SEGMENTS=%-6s    " "$K"
	sizes "$OUT/seg_$K.o"
done

echo
echo "Expect: .text constant across MAX_SEGMENTS (algorithm does not grow with"
echo "the blob); .bss exactly 16 B per segment (two u64 id tables)."
