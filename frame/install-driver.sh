#!/bin/bash
# frame/install-driver.sh [ssh-host] [uninstall] : build driver_framectl natively on the headset (SteamOS ships gcc)
# into ~/framectl and register it with the headset's SteamVR. Needed for `frame-ctl hs ...`: controllers spawned there
# reach the PC through vrlink like real ones, so flat games' Steam gamepad works with nobody holding the controllers.
# The build-specific dashboard hooks stay off there (unknown SteamVR build); input override and spawn work.
# The new .so is moved into place (never overwritten while vrserver has it mapped). Loads on the headset's next vrserver start (`ssh frame 'kill $(pgrep -x vrserver)'`; the link restarts it).
set -eu
H=${1:-frame}; cd "$(dirname "$(readlink -f "$0")")/.."
REG=/opt/steamvr/bin/linuxarm64/vrpathreg
if [ "${2:-}" = uninstall ]; then
  ssh "$H" "$REG removedriver ~/framectl >/dev/null 2>&1; rm -rf ~/framectl ~/framectl-src; echo uninstalled"
  exit
fi
[ -f driver/hand_poses.h ] || { echo "run ./build.sh first (generates driver/hand_poses.h)"; exit 1; }
ssh "$H" 'mkdir -p ~/framectl-src ~/framectl/bin/linuxarm64 ~/.local/state/frame-ctl'
scp -q driver/framectl.cpp driver/hand_poses.h third_party/openvr-v2.15.6/openvr_driver.h "$H":framectl-src/
scp -q driver/framectl/driver.vrdrivermanifest "$H":framectl/
ssh "$H" "cd ~/framectl-src && g++ -std=c++17 -O2 -shared -fPIC -fvisibility=hidden -Wall -I. framectl.cpp \
  -o ~/framectl/bin/linuxarm64/.new.so -lpthread && mv ~/framectl/bin/linuxarm64/.new.so ~/framectl/bin/linuxarm64/driver_framectl.so && $REG adddriver ~/framectl >/dev/null 2>&1; \
  grep -q framectl ~/.config/openvr/openvrpaths.vrpath && echo installed"
