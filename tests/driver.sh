#!/bin/bash
# frame-ctl reaches the driver; HMD components plus both (virtual) controllers listed
s=$(frame-ctl raw spawn >/dev/null; sleep 2; frame-ctl status) || exit 1
for want in 'ok framectl' $'hmd\t/proximity' $'left\t/input/trigger/value' $'right\t/input/a/click'; do
  grep -qF "$want" <<< "$s" || { echo "FAIL: missing '$want'"; exit 1; }; done
echo PASS
