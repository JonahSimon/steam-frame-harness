# Writing a test script for your game

`frame-ctl` is the general part. A script that tests one particular game, mod list or network setup is yours:
keep it outside this repo (`~/.local/bin`, a dotfiles repo) or on a local branch that never gets pushed (see
"Where your scripts live" below). This page is the pattern those scripts follow, so they behave well on a shared
headset and leave clean evidence.

## The shape

Every game test does the same six things:

1. **Check the headset is free.** `frame-ctl busy <gameid>` exits 0 and prints a reason when someone else is using
   it (another agent's run, a game on the PC or on the headset, a `vr-restart` in progress). Stop then. Never
   set `FRAMECTL_FORCE` in a script: it gets past the check by killing whatever is running.
2. **Get VR up.** `frame-ctl worn on` keeps an unworn headset awake (restarts nothing), and `frame-ctl
   vr-restart` brings up PC SteamVR, waits for the headset stream and spawns the virtual controllers.
3. **Start a run and launch.** `frame-ctl run start <label>`, then `frame-ctl launch <rungameid>`, which waits
   for the game's frames and keeps the headset dashboard out of the way. Flat games streamed to the headset use
   `frame-ctl stream <appid>`; games running on the headset itself use `frame-ctl native ...`.
4. **Wait for evidence, not for time.** Poll for something the game or SteamVR writes: compositor stats
   (`vrcmd --stats`, `average_application_gpu_time_ms` above 0 means frames), a line in the game's log, a
   window title. Give each wait a timeout and a distinct result for each way it can end.
5. **Act and capture.** Inputs (`press`, `stick`, `pose`), then `frame-ctl shot` (the headset view) or
   `frame-ctl shot out.png "<window title>"`. Screenshots and `frame-ctl run note` lines land in the run folder.
6. **Print one result line and clean up.** `frame-ctl run stop`, then `frame-ctl cleanup` unless a person asked
   to keep the game up. A script that leaves the game running for inspection should say so in its header, so
   whoever runs it knows to clean up after.

## Result values

Make the outcome impossible to confuse with "the script didn't get that far". Use distinct words, and print
them on one tab-separated line you can append to a results file:

| Result | Meaning |
|---|---|
| `PASS` / `FRAMES` | the thing you waited for happened |
| `FAIL` | it didn't, within the timeout |
| `EXITED` | the game process went away before that |
| `NOSTREAM`, `VRFAIL` | the setup step failed, so the game was never tested |
| `BUSY` (exit 3) | the headset was in use; nothing was touched |
| `INVALID` | the evidence belongs to something else (a stale log line, another process) |

## Template

```bash
#!/bin/bash
# mygame-test [label]: launch MyGame, wait for VR frames, screenshot, one result line.
# Leaves nothing running (frame-ctl cleanup at the end). Results: ~/mygame-tests/results.tsv
set -u
GAMEID=123456          # Steam appid, or the 64-bit rungameid of a non-Steam shortcut
EXE="MyGame.exe"       # process name, to tell "exited" from "still loading"
OUT=~/mygame-tests; mkdir -p "$OUT"; L=${1:-test}
VRCMD=${STEAMVR:-~/.local/share/Steam/steamapps/common/SteamVR}/bin/linux64/vrcmd

why=$(frame-ctl busy "$GAMEID") && { echo "BUSY: $why"; exit 3; }
frame-ctl worn on >/dev/null
frame-ctl vr-restart >/dev/null || { printf '%s\t%s\tVRFAIL\n' "$(date +%F_%T)" "$L" | tee -a "$OUT/results.tsv"; exit 1; }
frame-ctl run start "mygame-$L" >/dev/null
frame-ctl launch "$GAMEID" &
t=$SECONDS; res=TIMEOUT
while [ $((SECONDS - t)) -lt ${TMO:-300} ]; do
  sleep 5
  [ $((SECONDS - t)) -gt 30 ] && ! pgrep -f "[${EXE:0:1}]${EXE:1}" >/dev/null && { res=EXITED; break; }
  "$VRCMD" --stats 2>/dev/null | grep -qE '"average_application_gpu_time_ms" : [1-9]' && { res=FRAMES; break; }
done
wait
[ $res = FRAMES ] && { sleep ${SETTLE:-20}; frame-ctl shot "$OUT/$(date +%H%M%S)-$L.png" >/dev/null; }
printf '%s\t%s\t%s\n' "$(date +%F_%T)" "$L" "$res" | tee -a "$OUT/results.tsv"
frame-ctl run stop >/dev/null
frame-ctl cleanup
```

The `[M]yGame.exe` bracket trick keeps `pgrep -f` from matching the script's own command line.

## Rules

- **Machine-specific values come from the environment** (`FRAMECTL_FRAME_HOST` for the headset's ssh alias,
  `STEAMVR`), never a hardcoded IP, user name or home path. Get the headset's address with
  `ssh -G "${FRAMECTL_FRAME_HOST:-frame}" | awk '/^hostname /{print $2}'` when you need it.
- **Scratch files go in `mktemp -d`** with a `trap` to remove them, not a fixed `/tmp/...` path that two runs
  share.
- **Never kill by broad pattern.** Quit the game by its exact process name (`pgrep -x`) or let `cleanup` do it
  (it finds games by their `SteamGameId`).
- **Don't restart the headset's SteamVR** from a script. It kills anything running on the headset. `worn` and
  `hs` don't need it.
- Waiting on a log: remember where it ended before launching (time or byte offset) and read only what came
  after, and include the rotated `.previous` file, so an old line can't pass the test.

## Where your scripts live

- **Outside the repo** (`~/.local/bin/mygame-test`) is simplest and can't leak into a push.
- **A local branch** if you want them versioned next to the harness: keep `main` general and put game- and
  machine-specific files on, say, `local`, which you never push. Commit general fixes on `main` and bring them
  over with `git merge main` while `local` is checked out (merge rather than rebase, so other agents' commit ids
  stay valid). Before pushing `main`, check nothing personal slipped in: `git diff origin/main main --stat`.
