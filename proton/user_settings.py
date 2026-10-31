# steam-frame-harness: PROTON_LOG only while a `frame-ctl run` is active (writes ~/steam-<appid>.log, collected by
# frame-ctl-logs). Symlink into a Proton/GE-Proton dir as user_settings.py; normal play is unaffected.
import os

user_settings = {}
try:
    run = open(os.path.expanduser("~/.local/state/frame-ctl/current-run")).read().strip()
    pid = int(open(os.path.join(run, ".collector.pid")).read())
    os.kill(pid, 0)  # a stale pointer from a crashed run must not turn on logging for normal play
    user_settings["PROTON_LOG"] = "1"
except (OSError, ValueError):
    pass
