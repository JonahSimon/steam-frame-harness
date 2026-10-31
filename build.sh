#!/bin/bash
# build.sh [install]: build driver_framectl + vrprobe. `install` also registers the driver with SteamVR (vrpathreg),
# symlinks frame-ctl into ~/.local/bin and turns on SteamVR's VR View mirror. Restart SteamVR afterwards.
set -eu
cd "$(dirname "$(readlink -f "$0")")"
SV=${STEAMVR:-$HOME/.local/share/Steam/steamapps/common/SteamVR}
OPENVR_TAG=v2.15.6  # pinned; bump deliberately
H=third_party/openvr-$OPENVR_TAG
mkdir -p $H driver/framectl/bin/linux64
for f in openvr_driver.h openvr.h; do
  [ -f $H/$f ] || curl -fsSL -o $H/$f https://raw.githubusercontent.com/ValveSoftware/openvr/$OPENVR_TAG/headers/$f
done
python3 tools/bake-hand-poses.py "$SV" > driver/hand_poses.h  # SteamVR's own hand poses, not redistributed
g++ -std=c++17 -O2 -shared -fPIC -fvisibility=hidden -Wall -I$H driver/framectl.cpp -o driver/framectl/bin/linux64/driver_framectl.so -lpthread
g++ -O2 -I$H probe/vrprobe.cpp -o probe/vrprobe -L"$SV/bin/linux64" -lopenvr_api -Wl,-rpath,"$SV/bin/linux64"
echo built
if [ "${1:-}" = install ]; then
  LD_LIBRARY_PATH="$SV/bin/linux64" "$SV/bin/linux64/vrpathreg" adddriver "$PWD/driver/framectl"
  mkdir -p ~/.local/bin ~/.local/state/frame-ctl && ln -sf "$PWD/frame-ctl" ~/.local/bin/frame-ctl
  pgrep -x vrserver >/dev/null && probe/vrprobe setbool steamvr showMirrorView 1 || echo "start SteamVR once and run: probe/vrprobe setbool steamvr showMirrorView 1"
  echo "installed; restart SteamVR (frame-ctl vr-restart)"
fi
