---
name: frame-ctl
description: Drive a Steam Frame VR headset and its controllers with nobody wearing it, to test PC VR games end to end (press buttons, move sticks, pose HMD/controllers, screenshot the headset view, read a game's button bindings, collect per-run logs). Use when asked to test, smoke-test, reproduce a bug in, or navigate a SteamVR game on the Frame.
---

# frame-ctl

The repo README has the details. This is the loop.

## Before you start

1. `frame-ctl doctor`. Every line should say `ok`; each FAIL names its fix. If the driver isn't answering, run
   `frame-ctl vr-restart` (starts SteamVR, waits for the headset stream, spawns the virtual controllers, hides
   the headset dashboard). If the stream won't come up, wake the headset (`frame-ctl wake`, `frame-ctl worn on`).
2. **One agent per headset.** Run `frame-ctl busy` first. If it names another agent's run or a game you didn't
   start, stop and report. While another agent's run is active, every state-changing frame-ctl command refuses
   (runs record their owner's session). Don't set FRAMECTL_FORCE to get past it: a `vr-restart` kills their game.
   `launch` warns when RAM is short (< 8 GB available); then free memory (`vr-prep on`) and run no parallel agents.

## Test

3. `frame-ctl run start <label>`. Everything until `run stop` lands in one folder.
4. `frame-ctl launch <steam app id or rungameid>`. It waits for game frames and runs `dash hide`. If inputs do
   nothing, the first suspect is the headset dashboard: `probe/vrprobe | head -1` must show
   `dashboard_visible=0 input_available=1`; if not, `frame-ctl dash hide`.
5. `frame-ctl bindings <app>` shows what every Frame input does in this game. Read it before pressing things.
6. Act, then look: `frame-ctl pose hmd level 0` (upright head), `frame-ctl press right trigger`,
   `frame-ctl stick left 0 1 2`, then `frame-ctl shot "" vrview` (the headset view) and read the image.
   Turn the head with `pose hmd level <deg>`. `frame-ctl watch` in another shell shows every injected input and
   every haptic pulse the game sends back.
7. Before blaming the harness, check what the game received. Game logs (script-extender plugins, the game's own
   log) tell "input missing" apart from "the game ignores it right now" (cutscene, menu, scripted lock).
8. `frame-ctl run stop`, and cite the run folder in your report.

## Clean up (always, even when the test failed)

9. `frame-ctl cleanup`. It stops your run, quits the game, clears every override, removes the virtual
   controllers, lets the headset's dashboard work again, lets the headset sleep, and quits SteamVR. Check that
   every line says `ok`. Use `--keep-vr` / `--keep-awake` only if the person asked you to leave it running for
   them. Never end a session with a game, SteamVR or the keep-awake override still running.

Your own test scripts: call `frame-ctl busy <gameid>` before touching SteamVR and stop if it prints a reason.

Bethesda VR games (Skyrim VR, Fallout 4 VR): `frame-ctl console "<window title>" "<command>"` types a console
command (one key at a time, lowercased); e.g. `frame-ctl console "Fallout4VR" "player.additem 21b3b 1"`.

Rules: keep the game's desktop window focused (`frame-ctl focus <window>`) while injecting input. Don't edit
SteamVR settings or Steam vdfs while they run. Don't run heavy parallel work next to a big modded game: the PC
can run out of memory and the OOM killer may take your process.
