#!/bin/bash
# frame/install.sh [ssh-host] [uninstall] : install the headset-side pieces over ssh (default host: frame).
# Needs: SteamOS on the Frame, ssh access as user steamos, passwordless sudo (or run the printed steps by hand).
# Installs /var/lib/agent-noprox (eeprom_console wrapper + saved real binary + prox_far value), agent-noprox.service
# (disabled; `agent-awake on` enables it) and ~/bin/agent-awake. /etc and /var persist across SteamOS updates.
# Also masks SteamOS's steam(vr)-web-debug-portforward units, which expose Steam's CEF debug port on the LAN;
# frame-cef reaches it through an ssh tunnel instead.
set -eu
H=${1:-frame}; cd "$(dirname "$(readlink -f "$0")")"
if [ "${2:-}" = uninstall ]; then
  ssh "$H" 'sudo systemctl disable --now agent-noprox 2>/dev/null; sudo rm -f /etc/systemd/system/agent-noprox.service; sudo rm -rf /var/lib/agent-noprox; rm -f ~/bin/agent-awake; sudo systemctl daemon-reload; sudo systemctl unmask steam-web-debug-portforward.service steamvr-web-debug-portforward.service; echo uninstalled'
  exit
fi
ssh "$H" 'mkdir -p ~/bin /tmp/fh && sudo -n true' || { echo "need passwordless sudo on $H (e.g. /etc/sudoers.d/zz-agents)"; exit 1; }
scp -q eeprom_console-wrapper.sh agent-noprox.service agent-awake "$H":/tmp/fh/
ssh "$H" 'set -e
  sudo mkdir -p /var/lib/agent-noprox
  if ! findmnt /usr/lib/deckard-eeprom/eeprom_console >/dev/null && [ ! -e /var/lib/agent-noprox/eeprom_console.real ]; then
    sudo cp /usr/lib/deckard-eeprom/eeprom_console /var/lib/agent-noprox/eeprom_console.real; fi
  sudo install -m755 /tmp/fh/eeprom_console-wrapper.sh /var/lib/agent-noprox/eeprom_console
  [ -f /var/lib/agent-noprox/prox_far ] || echo 1 | sudo tee /var/lib/agent-noprox/prox_far >/dev/null
  sudo install -m644 /tmp/fh/agent-noprox.service /etc/systemd/system/agent-noprox.service
  install -m755 /tmp/fh/agent-awake ~/bin/agent-awake
  sudo systemctl daemon-reload
  sudo systemctl mask --now steam-web-debug-portforward.service steamvr-web-debug-portforward.service
  rm -rf /tmp/fh; echo "installed; run: ssh '"$H"' ~/bin/agent-awake on"'
