#!/usr/bin/env python3
"""frame-sensors.py [interval_s] : headset-side sampler, one CSV row per interval on stdout (header first).
Runs on the Frame (`ssh frame python3 -u - < frame-sensors.py`). frame-ctl-logs streams it into <run>/frame-sensors.csv.
Sensor paths come from sasaken1102r/frame-perf-overlay (MIT, headset/sensors.cpp + wifi_link.cpp), SteamOS 0.4/0.5.
Temps in C, power in W, clocks in MHz. throttle_* = thermal cooling_device cur_state (0 = not throttled).
"""
import glob, os, re, subprocess, sys, time

IV = float(sys.argv[1]) if len(sys.argv) > 1 else 5


def rd(p):
    try:
        with open(p) as f: return f.read().strip()
    except OSError: return ""


def num(p, scale=1.0):
    try: return float(rd(p)) / scale
    except ValueError: return None


zones = {rd(z + "/type"): z + "/temp" for z in glob.glob("/sys/class/thermal/thermal_zone*")}
cpu_z = [p for t, p in zones.items() if t.startswith("cpu") and not t.startswith("cpuss")]
gpu_z = [p for t, p in zones.items() if t.startswith("gpuss")]
ddr_z = next((p for t, p in zones.items() if t.startswith("ddr")), "")
cool = {rd(c + "/type"): c + "/cur_state" for c in glob.glob("/sys/class/thermal/cooling_device*")}
cpu_cool = [p for t, p in cool.items() if t.startswith("cpufreq-cpu")]
gpu_cool = [p for t, p in cool.items() if t.startswith("devfreq-") and "gpu" in t]
rails = {}  # label -> power*_input (µW)
for h in glob.glob("/sys/class/hwmon/hwmon*"):
    if rd(h + "/name").startswith("max34417"):
        for p in glob.glob(h + "/power*_input"):
            lab = rd(p.replace("_input", "_label")) or os.path.basename(p)
            if lab != "unused": rails[lab] = p
RAILS = sorted(rails, key=lambda l: (l != "vph", l))
pols = sorted(glob.glob("/sys/devices/system/cpu/cpufreq/policy*"), key=lambda p: int(re.sub(r"\D", "", p)))
gpu_df = next(iter(glob.glob("/sys/class/devfreq/*gpu*")), "")
bat = next((p for p in glob.glob("/sys/class/power_supply/*") if rd(p + "/type") == "Battery"), "")
chg = next(iter(glob.glob("/sys/class/power_supply/*charger*")), "")
vadc = next((d for d in glob.glob("/sys/bus/iio/devices/iio:device*")
             if rd(d + "/name") == "c400000.spmi:pmic@0:vadc@9000"), "")
VADC = ["display_left_therm", "pcba_at_fan_exhaust", "pcba_at_heat_sink_fins"]


def wifi(dev, ack):
    """(signal dBm, tx Mbit/s). AP side (wlanap = PC dongle) reports signal 0, so use avg ack signal there."""
    try: out = subprocess.run(["iw", "dev", dev, "station" if ack else "link"] + (["dump"] if ack else []),
                              capture_output=True, text=True, timeout=3).stdout
    except (OSError, subprocess.TimeoutExpired): return None, None
    s = re.search(r"avg ack signal:\s*(-?\d+)" if ack else r"\bsignal:\s*(-?\d+)", out)
    b = re.search(r"tx bitrate:\s*([\d.]+)", out)
    return (int(s.group(1)) if s else None), (float(b.group(1)) if b else None)


def ticks():
    f = rd("/proc/stat").split("\n")[0].split()[1:]
    v = list(map(int, f)); return sum(v), v[3] + v[4]


cols = (["time", "cpu_util_pct", "cpu_temp_max", "gpu_temp_max", "ddr_temp", "skin_display", "skin_exhaust",
         "skin_heatsink", "bat_temp", "throttle_cpu", "throttle_gpu"]
        + [f"{os.path.basename(p)}_mhz" for p in pols] + [f"{os.path.basename(p)}_cap_mhz" for p in pols]
        + ["gpu_mhz"] + [f"w_{r}" for r in RAILS]
        + ["bat_pct", "bat_status", "bat_w", "charger_online", "dongle_dbm", "dongle_mbps", "wifi_dbm", "wifi_mbps",
           "mem_avail_mb"])
print(",".join(cols), flush=True)
prev = ticks()
while True:
    time.sleep(IV)
    cur = ticks(); dt = cur[0] - prev[0]
    util = 100 * (1 - (cur[1] - prev[1]) / dt) if dt else None; prev = cur
    mx = lambda ps: max((num(p, 1000) or -999 for p in ps), default=None)
    bv, bi = num(bat + "/voltage_now", 1e6), num(bat + "/current_now", 1e6)
    mem = re.search(r"MemAvailable:\s+(\d+)", rd("/proc/meminfo"))
    row = ([time.strftime("%F %T"), util, mx(cpu_z), mx(gpu_z), num(ddr_z, 1000)]
           + [num(f"{vadc}/in_temp_{n}_input", 1000) if vadc else None for n in VADC]
           + [num(bat + "/temp", 10), max((int(num(p) or 0) for p in cpu_cool), default=None),
              max((int(num(p) or 0) for p in gpu_cool), default=None)]
           + [num(p + "/scaling_cur_freq", 1000) for p in pols] + [num(p + "/scaling_max_freq", 1000) for p in pols]
           + [num(gpu_df + "/cur_freq", 1e6) if gpu_df else None] + [num(rails[r], 1e6) for r in RAILS]
           + [num(bat + "/capacity"), rd(bat + "/status"), bv * bi if bv is not None and bi is not None else None,
              rd(chg + "/online") if chg else None, *wifi("wlanap", True), *wifi("wlan0", False),
              int(mem.group(1)) // 1024 if mem else None])
    print(",".join("" if v is None else f"{v:.2f}" if isinstance(v, float) else str(v) for v in row), flush=True)
