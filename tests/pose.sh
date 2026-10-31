#!/bin/bash
# controller + HMD pose overrides read back by vrprobe; HMD worn (activity 1 + proximity bit) with nobody in it
P=$(dirname "$(readlink -f "$0")")/../probe/vrprobe
frame-ctl raw 'pose left world 0.25 0.5 -0.75' >/dev/null; frame-ctl pose hmd level 90 >/dev/null; frame-ctl raw 'set hmd /proximity 1' >/dev/null
sleep 1; o=$($P scene 2)
frame-ctl raw 'pose left head -0.2 -0.35 -0.35' >/dev/null
echo "$o" | grep -E '^[0-9]+ class=2 role=1' | grep -q 'pos=0.250,0.500,-0.750' || { echo "FAIL: left pose"; echo "$o"; exit 1; }
echo "$o" | grep -q 'hmd_activity=1' || { echo "FAIL: activity"; exit 1; }
echo "$o" | grep -E '^0 class=1' | grep -q 'pressed=0x80000000' || { echo "FAIL: proximity bit"; echo "$o"; exit 1; }
echo PASS
