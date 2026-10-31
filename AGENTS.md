# Agent instructions

This repo lets an AI agent test PC VR games on a Valve Steam Frame with nobody wearing it.

- **Using the harness** (testing a game): follow `skills/frame-ctl/SKILL.md`. Claude Code users can install it as
  a skill: `ln -s "$PWD/skills/frame-ctl" ~/.claude/skills/frame-ctl`. Other agents: put that file in your
  instructions.
- **Always clean up:** finish every session with `frame-ctl cleanup` (quits the game and SteamVR, clears
  overrides, lets the headset sleep), even after a failed test.
- **One agent per headset.** If a run or a game is already active, stop and report instead of restarting VR.

Working on the code:

- `./build.sh` builds the driver and probe. A driver change needs `frame-ctl vr-restart`, which ends any running
  game.
- Tests need SteamVR up with the headset streaming: `tests/driver.sh`, `tests/input.sh`, `tests/pose.sh`,
  `tests/run-folder.sh <run dir>`.
- `driver/framectl.cpp` patches SteamVR driver vtables. The dashboard hooks are tied to one SteamVR build (see
  README); verify on the new build before widening the version gate.
- Keep machine-specific values in env vars (`FRAMECTL_*`), never hardcoded.
