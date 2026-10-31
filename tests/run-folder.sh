#!/bin/bash
# run-folder.sh [run-dir]: a finished `frame-ctl run` folder holds every log family (default: newest finished run).
# Proton and game logs are only checked when the run launched a Proton game (proton/ or game/ non-empty).
d=${1:-$(ls -dt ~/frame-ctl-runs/*/ | while read x; do [ -f "$x/MANIFEST.txt" ] && echo "$x" && break; done)}
fail=0; need(){ compgen -G "$d/$1" >/dev/null || { echo "missing: $1"; fail=1; }; }
need 'pc-steam-logs/vrserver.txt'; need 'pc-steam-logs/vrclient_*.txt'
need 'stats/*.json'; need 'frame/journal-system.log'; need 'frame/vrserver.txt'; need 'frame/XRService-*.log'
need 'input-timeline.tsv'; need 'shots/*'; need 'framectl-driver.log'
[ -n "$(ls -A "$d/game" 2>/dev/null)" ] && need 'proton/steam-*.log'
[ $fail = 0 ] && echo "PASS ($d)" || { echo "FAIL ($d)"; exit 1; }
