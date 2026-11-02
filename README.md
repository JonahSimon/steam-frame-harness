# steam-frame-harness

Drive a Valve Steam Frame (PC VR over Wi-Fi via SteamVR's vrlink) with nobody wearing it, so an agent or a script
can test VR games end to end: press controller buttons, move sticks, pose the HMD and controllers, keep the
headset awake and "worn", capture what the headset shows, see what each button does in a game's bindings, and log
everything per run.

> **Unofficial.** Not affiliated with or endorsed by Valve. It patches SteamVR driver interfaces at runtime, uses
> SteamVR internals found by disassembly, and runs JavaScript inside the Steam client on your headset. It can
> break with any SteamVR or SteamOS update. Use it on your own hardware and accounts.

Tested on CachyOS (KDE Plasma Wayland), SteamVR 2.18.4 (build 1792185270), SteamOS 0.4.5 on the Frame,
Fallout 4 VR and Skyrim VR (Proton).

## Controller emulation

`frame-ctl spawn` adds a virtual left and right controller that SteamVR treats as Steam Frame controllers:

- the same identity as the real ones (`Steam_Frame_Controller_Left/Right`, controller type `frame_controller`,
  input profile, render models, tracking system `cv`), so games load their real Frame bindings and hand models;
- every input component the real controllers have: `system menu view a b x y bumper` (click/touch),
  `dpad_up/down/left/right` (left), `trigger` and `grip` (click/touch/value), `thumbstick` (click/touch/x/y),
  `thumbrest` (touch), plus a haptic output;
- the Frame's layout quirk comes from SteamVR's own profile: A/B/X/Y and menu are right-hand inputs, the left
  controller has a d-pad and view button that the profile mirrors to them.

While spawned, real controllers report as disconnected to apps, so the hand roles always go to the virtual pair
(`despawn` gives them back). You can also override inputs of the real headset and controllers directly
(`raw set <dev> <component> <value>`).

- a finger skeleton (`/input/skeleton/left|right`), blended per finger between SteamVR's own open-hand and fist
  reference poses (baked at build time from SteamVR's `hand_right_poses.glb`): index follows the trigger,
  middle/ring/pinky the grip, the thumb any touched button. `frame-ctl fingers <hand> t i m r p` sets curls
  directly; `fingers <hand> off` goes back to input-driven. Verified in Boneworks (curls show on the hands).

**Not emulated:** eye tracking, battery state.

Debugging button layouts:

- `frame-ctl bindings [app]` prints what every Frame input does in the binding file SteamVR loaded for the app
  (default: the most recent game), per action set. Legacy apps show the legacy button they map to.
- `frame-ctl watch` streams real button changes from the physical headset/controllers, injected input and haptic
  pulses the game sends, so you can press a real button and see the component path.
- `frame-ctl press <hand> <button>` then a screenshot shows what the game does with it.

## Games without Frame bindings

Many games ship bindings only for Index, Touch, Vive and WMR controllers. SteamVR then converts one for the Frame,
preferring Oculus Touch, which can lose per-finger input and leave buttons in odd places (Boneworks: every finger
follows the grip). `tools/frame-binding` converts the game's Index binding into a native Frame binding with
SteamVR's own Index-to-Frame button table, maps finger curls to trigger and grip, and keeps the game in its Index
hand mode:

```bash
G=~/.local/share/Steam/steamapps/common/BONEWORKS/BONEWORKS
tools/frame-binding $G/knuckles_bindings.json --install $G/actions.json   # backs up actions.json
frame-ctl bindings 823500                                                 # check what loaded
```

A Steam "verify files" or game update removes it; rerun the command.

Find candidates across your whole library without launching anything: `tools/frame-compat` (about 20 s) sorts
installed VR games into native / convertible / touch-only / legacy / openxr and prints the `frame-binding` command
for each convertible one.

## Flat (non-VR) games

PC Steam turns the Frame controllers into one gamepad ("Steam Frame Controllers", then a uinput Xbox pad for the
streamed game), fed from the controllers vrlink relays into PC SteamVR. Proven 2026-11-01 with Project Reclaimer
(non-Steam shortcut): an injected A opened its Server Browser.

- **Use headset-side virtual controllers** (`frame/install-driver.sh`, then `frame-ctl hs spawn`). vrlink relays
  them to the PC as real controllers, so the pad works with nobody holding the real ones. Inject with `frame-ctl hs
  press right a` (or PC-side `frame-ctl press`, which then overrides the relayed devices; a press needs touch + click).
- **Never `frame-ctl spawn` on the PC for a flat game.** PC-spawned controllers don't feed the pad, and spawning them
  destroys and recreates it; nothing reaches the game until `despawn`.
- **The headset dock eats the input.** While the headset dashboard is open the pad gets nothing; launching a game
  leaves it open. Close it (`frame-ctl hs press right system` toggles it).
- Proven for a non-Steam shortcut (Proton), a Steam game under Proton and a Linux-native Steam game.
- **Controllers with no hand role build no pad.** After the headset's VR stack restarts (`agent-awake on` does
  that), the PC keeps the old relayed controllers with role 0 (`probe/vrprobe`); restart PC SteamVR.
- **Launch from the headset**: `frame-ctl stream <appid>` (Steam or shortcut appid). A launch started on the PC
  waits on a prompt that shows nowhere.
- `tools/padwatch` logs every gamepad device and event on the PC; `tools/frame-ui-shot` screenshots the headset's
  own Steam UI (VR View never shows it).
- `frame-ctl cleanup` also clears and despawns the headset-side driver: its virtual controllers hide the real ones.

## How it works

- **`driver_framectl`** (`driver/framectl.cpp`) is a SteamVR driver loaded next to vrlink. It patches the vtables
  of the shared `IVRDriverInput_005` and `IVRServerDriverHost_006` objects, so it can override any input component
  or pose another driver reports, and it adds the virtual controllers. vrserver gives each driver its own
  interface object, so updates are replayed through the owning driver's object. Control is text over UDP
  `127.0.0.1:47823`. Every command, real input change and haptic event is logged to
  `~/.local/state/frame-ctl/driver.log` (rotated at 20 MB).
- **`frame-ctl`**: the CLI (`frame-ctl help`).
- **`frame-ctl-logs`**: per-run collector behind `frame-ctl run`.
- **`frame-cef`**: runs JS in the headset's Steam client through an ssh tunnel to its CEF debug port.
- **`probe/vrprobe`**: independent OpenVR client for checks (poses, activity, dashboard state, controller state;
  `vrprobe scene 3` takes scene focus to read legacy input).
- **`frame/`**: headset-side pieces and `frame/install.sh`.

### The dashboard problem (why games got no input)

When nobody uses the headset, the Frame's Steam client reopens its dashboard right after every hide (it sends
`ShowOverlay#1 valve.steam.gamepadui.main` to its local SteamVR). vrlink mirrors that dashboard to the PC, and
while it is open the PC dashboard owns all input (`IsInputAvailable() == false`), so games ignore every
controller. `frame-ctl dash hide` injects a small hook into the headset's Steam JS that drops that one message,
then hides the dashboard. A Steam restart on the headset drops the hook; while `dash hide` is in effect the
headset's `framectl-dashguard` user unit puts it back within 3 s and hides the dashboard again (`dash allow` and
`cleanup` turn that off). A game launch re-opens the dashboard, so `frame-ctl launch` and `vr-restart` run
`dash hide` for you.

The driver's `nodash on` also drops vrlink's PC-side reopen call (`IVRServerInternal_XXX` slot 57; `dash close`
calls slot 58). Those slots were found by disassembling `driver_vrlink.so` from SteamVR 2.18.4, so the driver
only installs them on that build (`bin/version.txt` = 1792185270). Set `FRAMECTL_FORCE_NODASH=1` in vrserver's
environment to try them on another build. `dash hide` alone is enough in practice.

## Requirements

PC: Linux, Steam + SteamVR, the Frame streaming over vrlink, `g++`, `curl`, `python3`,
[`uv`](https://docs.astral.sh/uv/) (for `frame-cef`), `ssh`, `xdotool`, `spectacle` (KDE; screenshots), optional
`imagemagick`. Headset: SteamOS with developer mode, an ssh login, passwordless sudo for the installer, and
**CEF remote debugging** enabled (developer settings).

`frame-ctl doctor` checks all of it.

## Setup

1. `./build.sh install`: fetches pinned OpenVR headers (v2.15.6), builds, registers the driver with
   `vrpathreg`, links `frame-ctl` into `~/.local/bin`, turns on SteamVR's VR View mirror window.
2. Headset: give it an ssh alias (default `frame`), then `frame/install.sh frame`. That installs the keep-awake
   toggle (`~/bin/agent-awake on|off|status`, a wrapper that fakes the proximity calibration so the idle sensor
   reads as worn), the `framectl-dashguard` user unit (see How it works), and masks SteamOS's `steam(vr)-web-debug-portforward` units, which otherwise expose Steam's
   debug port on your LAN. `frame/install.sh frame uninstall` reverts everything.
3. `frame-ctl vr-restart`, then `frame-ctl doctor`.
4. Optional Proton logs during runs: symlink `proton/user_settings.py` into your Proton/GE-Proton dir as
   `user_settings.py`. It sets `PROTON_LOG=1` only while a `frame-ctl run` is active.

Config (env): `FRAMECTL_FRAME_HOST` (ssh alias), `FRAMECTL_AWAKE_CMD`, `FRAMECTL_SHOT_WINDOW` (a game desktop
window title for `focus`/`shot`; default captures the headset view), `FRAMECTL_FRAME_CEF=host:port` (skip the
tunnel), `FRAMECTL_LOG_INTERVAL` (seconds between run screenshots, default 60), `FRAMECTL_GAME_LOG_DIRS`,
`STEAMVR` (SteamVR dir).

## Using it with an AI agent

Give your agent `skills/frame-ctl/SKILL.md` (Claude Code: `ln -s "$PWD/skills/frame-ctl" ~/.claude/skills/`);
`AGENTS.md` covers other agents and people working on the code. The skill tells the agent to check `doctor`
first, never take over a headset someone else is using, and finish with `frame-ctl cleanup`.

## Typical test

```bash
frame-ctl vr-restart                      # SteamVR up, headset streaming, virtual controllers, dash hidden
frame-ctl run start smoke                 # ~/frame-ctl-runs/<ts>-smoke/
frame-ctl launch 450390                   # Steam app id or rungameid; waits for frames (or a start stage), dash hide
frame-ctl pose hmd level 0                # upright head, looking ahead
frame-ctl press right trigger             # menus usually confirm on the trigger
frame-ctl stick left 0 1 2                # walk forward 2 s
frame-ctl shot "" vrview                  # what the headset shows
frame-ctl run stop                        # collect logs
frame-ctl cleanup                         # quit game + SteamVR, clear overrides, let the headset sleep
```

Commands: `press|hold|release <hand> <btn>`, `trigger|grip <hand> <0..1|off>`, `stick <hand> <x> <y> [secs]|off`,
`pose <hmd|left|right> off|hold|level <yaw> [height]|world x y z [q]|head dx dy dz [q]|x y z [q]` (q =
`qw qx qy qz`), `worn on|off`, `wake`, `dash hide|allow|status`, `shot [file] [window|vrview]`, `focus [window]`,
`fingers <hand> t i m r p|off`, `spawn|despawn`, `clear`, `bindings [app]`, `busy [gameid]`, `watch`, `doctor`, `cleanup [--keep-vr] [--keep-awake]`, `status`, `run start|stop|note`, `raw <line>`.

Driver protocol (UDP, one line in, text out): `ping`, `list`, `poses`, `set <dev> <component> <value>`,
`rel <dev|all> <component|all>`, `pose ...`, `spawn`, `despawn`, `nodash on|off`, `dash close`, `clear`.

## Run folders

`frame-ctl run start <label>` creates `~/frame-ctl-runs/<timestamp>-<label>/`; `run stop` fills it:
`input-timeline.tsv` (every frame-ctl command, ms timestamps; `run note` adds markers), `stats/` (`vrcmd --stats`
each interval), `frame-sensors.csv` (headset temps, thermal throttle state, CPU/GPU clocks and caps, power rails,
battery, dongle/Wi-Fi signal every `FRAMECTL_SENSOR_INTERVAL` s, default 5; paths from frame-perf-overlay), `shots/` (JPEG headset view each interval), `pc-steam-logs/` (only what SteamVR/Steam wrote during
the run), `framectl-driver.log`, `pc-journal-user.log`, `frame/` (headset Steam/SteamVR logs, newest XRService log,
system and user journal), `proton/`, `game/` (logs under Proton prefixes' `My Games` that changed), `MANIFEST.txt`.
Only one run can be active: there is one headset.

## Tests

With SteamVR up and the headset streaming: `tests/driver.sh`, `tests/input.sh` (injected values read back by
`vrprobe`), `tests/pose.sh`, and `tests/run-folder.sh [dir]` after a run.

## Gotchas

- Games ignore controller input while their desktop window is unfocused. `shot` hands focus back after capturing;
  call `frame-ctl focus <window>` after doing anything else on the desktop.
- VR View is the headset view (overlays, menus). SteamVR's "Headset Mirror" captures black on this setup.
- Games lock movement in scripted scenes (e.g. Fallout 4's pre-war intro until character creation is finished).
  Check the game's own logs before blaming the harness.
- Spawn the virtual controllers before starting a game: some games (Skyrim VR) only pick up controllers that
  exist at startup. `frame-ctl launch` and `vr-restart` handle this; `spawn` warns if a game is already running.
- Don't `despawn` while a game runs: controllers disappearing and coming back crashed Fallout 4 VR (a mod read a
  null pointer). `despawn` refuses unless `FRAMECTL_FORCE=1`.
- Driver changes need a SteamVR restart, which ends the running game.
- One agent at a time. `vr-restart` and `launch` refuse while another Steam game is running or another
  `vr-restart` is in progress (`frame-ctl busy`); your own scripts should call `frame-ctl busy <gameid>` first.

## Game notes

- **Fallout 4 VR with FRIK**: the Pip-Boy opens when you look at your left wrist (`PipBoyOpenWhenLookAt` in
  `FRIK_Config/FRIK.ini`), not on a button. Working pose: `frame-ctl pose hmd level 0`, then the left hand at the
  head position + (-0.03, -0.15, -0.32) m with `pose left world x y z 0.2706 0.6533 -0.2706 -0.6533`. FRIK.log
  logs "Open Pipboy by looking at". Without FRIK, FO4VR's legacy binding puts the pause menu on left d-pad.
- **Fallout 4 / Skyrim console** from the PC keyboard: type slowly (`xdotool type --delay 250`); faster typing
  drops characters, and `xdotool key --window` events are ignored.
- **Boneworks**: no Frame binding ships; use `tools/frame-binding` (see above).

## Overhead

Measured 2026-10-31 with `probe/vrprobe timing` (per-frame compositor GPU time), Fallout 4 VR (Gingas, HD
profile), fixed head pose in one interior, RX 6900 XT, 40 to 60 s per condition:

| Condition | median ms | mean ms | p95 ms |
|---|---|---|---|
| Baseline (driver loaded, virtual controllers, VR View open) | 5.90 | 8.13 | 14.9 |
| Driver loaded, controllers despawned | 5.76 | 7.96 | 14.6 |
| VR View closed | 5.71 | 8.19 | 15.1 |
| Run logging (stats + JPEG every 20 s) and Proton logging | 5.96 | 8.20 | 15.1 |
| Proton logging only | 5.92 | 8.11 | 14.8 |

Every difference is within run-to-run noise (about 0.2 ms), and no condition dropped frames. The VR View mirror
is the only consistent one, about 0.2 ms on the median, so `cleanup` closes it. Wine debug logging
(`PROTON_LOG=1`) showed no cost in this game.

## Known limits (v0.1)

Linux PC only, KDE for screenshots, SteamVR 2.18.4 tested, finger skeleton is a two-pose blend (no splay), no eye tracking, the dashboard fix
depends on the headset's Steam JS and may break with a SteamOS update.

## Layout

Clone: `git clone https://github.com/JonahSimon/steam-frame-harness`

`driver/` SteamVR driver, `probe/` OpenVR probe, `frame/` headset-side scripts, `tests/`, `frame-ctl`,
`frame-ctl-logs`, `frame-cef`, `proton/` run-scoped Proton logging, `skills/frame-ctl/` agent skill.

License: MIT.
