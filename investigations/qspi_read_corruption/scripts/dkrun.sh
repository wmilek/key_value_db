#!/bin/bash
# dkrun.sh <build-dir> <logfile> <done-regex> <timeout-s>
#
# Capture-then-flash, with the reader count asserted. Two concurrent readers on
# the console tty split the byte stream and silently shred the log, so refuse to
# start unless exactly one is attached.
set -u

BUILD=$1; LOG=$2; DONE=$3; TMO=$4
# Optional 5th arg: regex of <err> lines that are expected and must NOT abort.
# The UBI backend probes volume ids before creating one, logging "No volumes
# present on device" for each probe — benign, and fatal to a naive filter.
IGNORE=${5:-$'\a'}
SN=${SN:-960115021}
PORT=${PORT:-/dev/ttyACM3}

readers() {
	local n=0 p
	for p in $(ls /proc | grep -E '^[0-9]+$'); do
		ls -l /proc/$p/fd 2>/dev/null | grep -q "$PORT" && n=$((n+1))
	done
	echo $n
}

# Kill any stale reader by pid found via /proc (never pkill -f: the pattern
# would match this script's own command line).
for p in $(ls /proc | grep -E '^[0-9]+$'); do
	ls -l /proc/$p/fd 2>/dev/null | grep -q "$PORT" && kill "$p" 2>/dev/null
done
sleep 1

[ "$(readers)" -eq 0 ] || { echo "ABORT: port still held"; exit 1; }

rm -f "$LOG"
stty -F $PORT 115200 cs8 -cstopb -parenb -crtscts -echo raw
nohup cat $PORT > "$LOG" 2>/dev/null &
CAT=$!
sleep 1
[ "$(readers)" -eq 1 ] || { echo "ABORT: expected 1 reader, got $(readers)"; kill $CAT; exit 1; }
echo "capture pid $CAT -> $LOG"

nrfutil device program --firmware "$BUILD/zephyr/zephyr.hex" \
	--options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE,reset=RESET_SYSTEM \
	--serial-number $SN 2>&1 | tail -2

end=$((SECONDS+TMO))
until grep -qE "$DONE" "$LOG" 2>/dev/null; do
	if grep -E "<err>|FATAL|CPU exception|BUS FAULT" "$LOG" 2>/dev/null \
	     | grep -qvE "$IGNORE"; then
		echo "!!! ERROR in log:"
		grep -E "<err>|FATAL|CPU exception" "$LOG" | grep -vE "$IGNORE" | head -5
		kill $CAT 2>/dev/null; exit 2
	fi
	[ $SECONDS -gt $end ] && { echo "TIMEOUT after ${TMO}s"; kill $CAT 2>/dev/null; exit 3; }
	sleep 5
done
kill $CAT 2>/dev/null
echo "=== DONE ($(wc -l < "$LOG") lines)"
