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

echo "capturing kernel trace to $OUT ..."
tracelogger -d1 -n 10 -f "$OUT"

echo
echo "== guardian user events =="
traceprinter -f "$OUT" | grep -i -E "usrevent|user" | head -n 60

echo
echo "== summary =="
printf "events in trace:      "; traceprinter -f "$OUT" | wc -l
printf "guardian misses:      "; traceprinter -f "$OUT" | grep -c "guardian late"
cat /dev/coldchain
