#!/bin/bash
# frame/install.sh [ssh-host] [uninstall] : install the headset-side pieces over ssh (default host: frame).
# Needs: SteamOS on the Frame, ssh access as user steamos, passwordless sudo (or run the printed steps by hand).
# Installs the user unit framectl-dashguard (re-installs the dash-hide hook after a Steam restart on the
# headset and re-holds /proximity while `frame-ctl worn on` is in effect; idle otherwise). /etc and /var persist across SteamOS updates.
# Also masks SteamOS's steam(vr)-web-debug-portforward units, which expose Steam's CEF debug port on the LAN;
# frame-cef reaches it through an ssh tunnel instead.
# Removes the old agent-awake keep-awake (agent-noprox eeprom wrapper), replaced by `frame-ctl worn` 2026-11-17.
set -eu
H=${1:-frame}; cd "$(dirname "$(readlink -f "$0")")"
if [ "${2:-}" = uninstall ]; then
  ssh "$H" 'sudo systemctl disable --now agent-noprox 2>/dev/null; sudo rm -f /etc/systemd/system/agent-noprox.service; sudo rm -rf /var/lib/agent-noprox; rm -f ~/bin/agent-awake; systemctl --user disable --now framectl-dashguard 2>/dev/null; rm -f ~/bin/framectl-dashguard ~/.config/systemd/user/framectl-dashguard.service ~/.local/state/framectl/dash-hide; sudo systemctl daemon-reload; sudo systemctl unmask steam-web-debug-portforward.service steamvr-web-debug-portforward.service; echo uninstalled'
  exit
fi
ssh "$H" 'mkdir -p ~/bin /tmp/fh && sudo -n true' || { echo "need passwordless sudo on $H (e.g. /etc/sudoers.d/zz-agents)"; exit 1; }
scp -q dashguard.py framectl-dashguard.service "$H":/tmp/fh/
ssh "$H" 'set -e
  if [ -e /etc/systemd/system/agent-noprox.service ]; then  # legacy agent-awake: unmount the wrapper, then remove it
    sudo systemctl disable --now agent-noprox
    if findmnt /usr/lib/deckard-eeprom/eeprom_console >/dev/null; then echo "eeprom_console still bind-mounted, left legacy files"; else
      sudo rm -f /etc/systemd/system/agent-noprox.service; sudo rm -rf /var/lib/agent-noprox; echo "removed legacy agent-noprox"; fi
  fi
  rm -f ~/bin/agent-awake
  install -m755 /tmp/fh/dashguard.py ~/bin/framectl-dashguard
  install -Dm644 /tmp/fh/framectl-dashguard.service ~/.config/systemd/user/framectl-dashguard.service
  systemctl --user daemon-reload; systemctl --user enable framectl-dashguard
  systemctl --user restart framectl-dashguard  # enable --now would leave an old copy running
  sudo systemctl daemon-reload
  sudo systemctl mask --now steam-web-debug-portforward.service steamvr-web-debug-portforward.service
  rm -rf /tmp/fh; echo "installed; for frame-ctl worn/hs/native also run frame/install-driver.sh '"$H"'"'
