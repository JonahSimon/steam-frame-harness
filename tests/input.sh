#!/bin/bash
# injected button/trigger/stick values are read back by an independent OpenVR client (vrprobe, scene focus)
P=$(dirname "$(readlink -f "$0")")/../probe/vrprobe
frame-ctl dash hide >/dev/null
frame-ctl hold right a >/dev/null; frame-ctl trigger left 0.8 >/dev/null; frame-ctl raw set left /input/thumbstick/y 1 >/dev/null
o=$($P scene 3); frame-ctl raw rel all all >/dev/null
echo "$o" | grep -q 'input_available=1' || { echo "FAIL: input not available (dashboard?)"; echo "$o"; exit 1; }
echo "$o" | grep -E '^[0-9]+ class=2 role=2' | grep -q 'pressed=0x80 ' || { echo "FAIL: right A"; echo "$o"; exit 1; }
echo "$o" | grep -E '^[0-9]+ class=2 role=1' | grep -q 'a0=0.00,1.00 a1=0.80' || { echo "FAIL: left stick/trigger"; echo "$o"; exit 1; }
echo PASS
