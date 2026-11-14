#!/bin/sh
# Run ON THE QNX TARGET while coldchaind is running.
# Captures a kernel event trace with tracelogger, then pulls out the
# guardian's own user events (state changes, actions, deadline misses).
#
#   sh /data/coldchain/trace.sh            # quiet system
#   echo stress_on > /dev/coldchain; sh /data/coldchain/trace.sh   # under CPU flood
#
# Open the .kev file in the Momentics IDE System Profiler (or QNX Toolkit
# for VS Code) to see the guardian thread pre-empting the flood threads.
OUT=${OUT:-/data/coldchain/coldchain.kev}
DIR=$(dirname "$0")

# traceprinter is not in the default mkqnximage image; deploy.bat copies it here
PRINTER=traceprinter
command -v traceprinter >/dev/null 2>&1 || PRINTER="$DIR/traceprinter"

# a previous capture that was interrupted keeps the trace buffers busy
slay -f tracelogger >/dev/null 2>&1
sleep 1

echo "capturing kernel trace to $OUT ..."
# normal mode (no -d1: that is daemon mode, which waits for the app to start
# tracing itself). Logs all kernel event classes until 10 buffers are full.
tracelogger -n 10 -f "$OUT" || exit 1
ls -l "$OUT"

if [ ! -x "$PRINTER" ] && ! command -v "$PRINTER" >/dev/null 2>&1; then
    echo "traceprinter not found: copy it to $DIR, or open $OUT in the System Profiler"
    exit 0
fi

echo
echo "== guardian user events =="
"$PRINTER" -f "$OUT" | grep -i -E "usrevent|user" | head -n 40

echo
echo "== summary =="
printf "events in trace:      "; "$PRINTER" -f "$OUT" | wc -l
printf "guardian late wakes:  "; "$PRINTER" -f "$OUT" | grep -c "guardian late"
cat /dev/coldchain
