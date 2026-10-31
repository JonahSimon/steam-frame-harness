# steam-frame-harness: PROTON_LOG only while a `frame-ctl run` is active (writes ~/steam-<appid>.log, collected by
# frame-ctl-logs). Symlink into a Proton/GE-Proton dir as user_settings.py; normal play is unaffected.
import os

user_settings = {}
run = os.path.expanduser("~/.local/state/frame-ctl/current-run")
if os.path.exists(run):
    user_settings["PROTON_LOG"] = "1"
