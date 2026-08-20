#!/bin/sh
# Build the persondb storage harness and reproduce the tables in
# doc/proposals/2026-08-16-persondb-case-performance.md.
#
# NOT a build of the project. It compiles the real lib/blob_db/blob_db.c and
# lib/containers/kvhash/kvhash.c against stub Zephyr headers and a RAM-backed
# NOR model, then replays app_cbor_persondb's storage traffic over it.
#
#   sh doc/proposals/persondb-perf/measure.sh            # the full table
#   sh doc/proposals/persondb-perf/measure.sh validate   # just the two checks
#
# A full run is ~10 configurations at ~30-60 s each.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
CC=${CC:-gcc}
OUT=${OUT:-$HERE/build}
mkdir -p "$OUT"

build() {   # $1 = CONFIG_BLOB_DB_MAX_PAYLOAD_LEN
	$CC -O2 -std=gnu11 -Wno-format \
	    -I"$HERE/stub" -I"$ROOT/include" -I"$ROOT/lib/blob_db" \
	    -DCONFIG_BLOB_DB_MAX_PAYLOAD_LEN="$1" \
	    -DCONFIG_BLOB_DB_SECTOR_BUF_SIZE=65536 \
	    -DCONFIG_BLOB_DB_LOG_LEVEL=1 \
	    -DCONFIG_BLOB_DB_IOSTATS=1 \
	    -o "$OUT/hb$1" \
	    "$HERE/harness.c" "$HERE/store_host.c" "$HERE/crc.c" \
	    "$ROOT/lib/blob_db/blob_db.c" \
	    "$ROOT/lib/containers/kvhash/kvhash.c"
}

for p in 4096 8192 16384; do
	build $p
done

# usage: run <payload> <people_maps> <people_buckets> <cred_maps> <cred_buckets>
#        -1 as a bucket count means "SIZE_MAX", what persondb.c asks for today.
run() {
	echo "== payload=$1  people=$2x$3  cred=$4x$5"
	"$OUT/hb$1" "${N:-10000}" 200 "$2" "$3" "$4" "$5" 0 | tail -n 8
	echo
}

if [ "$1" = "validate" ]; then
	echo "-- against RESULTS.md 8a (nRF5340-DK, 1 000 persons):"
	echo "   expect check 112.1 flash ops / 10 030 B, byid 53.8 / 5 135 B"
	N=1000 run 4096 16 -1 1 -1
	echo "-- against RESULTS.md 4 (native_sim, 10 000 persons):"
	echo "   expect check 274 flash ops / 13.4 KB, byid 131, put 815"
	N=10000 run 4096 16 -1 1 -1
	exit 0
fi

echo "### the shipped configuration"
run 4096 16 -1 1 -1

echo "### directory sizing only (no library config change)"
run 4096 32 256 2 256
run 4096 32 256 4 128
run 4096 64 128 8 64

echo "### plus a larger bucket payload ceiling"
run 8192 16 256 4 128
run 8192 32 128 4 128
run 16384 16 128 4 64
run 16384 32 64 8 32

echo "### bucket census and the insert fast path (shipped config)"
HB_CENSUS=1 "$OUT/hb4096" "${N:-10000}" 200 16 -1 1 -1 0 | grep census
HB_INSERT_FAST=1 "$OUT/hb4096" "${N:-10000}" 200 16 -1 1 -1 0 | tail -n 3
