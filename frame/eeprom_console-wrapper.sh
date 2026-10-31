#!/bin/sh
# Bind-mounted over /usr/lib/deckard-eeprom/eeprom_console by agent-noprox.service. The Frame's SteamVR (cv driver)
# reads the proximity calibration "prox_far" through this tool; reporting a tiny value (default 1) makes the idle
# sensor reading count as "worn", so the headset stays awake and games get focus with nobody wearing it.
# Every other eeprom_console call passes through to the real binary.
if [ "$1 $2" = "get prox_far" ]; then /var/lib/agent-noprox/eeprom_console.real "$@" >/dev/null; cat /var/lib/agent-noprox/prox_far; exit 0; fi
exec /var/lib/agent-noprox/eeprom_console.real "$@"
