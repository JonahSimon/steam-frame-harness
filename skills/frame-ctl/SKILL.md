---
name: frame-ctl
description: Drive a Steam Frame VR headset and its controllers with nobody wearing it, to test PC VR games end to end (press buttons, move sticks, pose HMD/controllers, screenshot the headset view, collect per-run logs). Use when asked to test, smoke-test, reproduce a bug in, or navigate a SteamVR game on the Frame.
---

# frame-ctl

Read `README.md` in the repo once. The loop:

1. `frame-ctl doctor`. Every line should say `ok`; each FAIL names its fix. If the driver is missing, run
   `frame-ctl vr-restart`. If the stream won't come up, wake the headset (`frame-ctl wake`, `frame-ctl worn on`).
   If `run start` refuses, another agent owns the headset: stop and report, don't take it over.
2. `frame-ctl run start <label>` before anything you want to keep. Everything after it lands in one folder.
3. `frame-ctl launch <steam rungameid>`. It waits for game frames and runs `dash hide`. If inputs do nothing, the
   first suspect is the dashboard: `probe/vrprobe | head -1` must show `dashboard_visible=0 input_available=1`.
   If not, run `frame-ctl dash hide` again.
4. Before pressing anything, run `frame-ctl bindings <app>` to see what each Frame input does in this game.
   Act, then look: `frame-ctl press right trigger`, `frame-ctl stick left 0 1 2`, then
   `frame-ctl shot "" vrview` (headset view) and read the PNG. Level the head first with
   `frame-ctl pose hmd level 0`. Turn to look around with `pose hmd level <deg>`.
5. Before blaming the harness, check what the game received. Its logs (e.g. FO4VR
   `F4SE/VR Controls Config.log` raw controller state, `PipboyTabsVR.log` menu opens) tell "input missing" apart
   from "game state ignores it" (cutscene, menu, scripted lock).
   `frame-ctl watch` in a second shell shows every injected input and every haptic pulse the game sends back.
6. `frame-ctl run stop`, then cite the run folder in your report.

Rules: keep the game's desktop window focused (`frame-ctl focus`) when injecting input. Don't edit SteamVR
settings or Steam vdfs while they run. Leave `frame-ctl clear` and `frame-ctl dash allow` set when you hand the
headset back to a human.
