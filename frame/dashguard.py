#!/usr/bin/env python3
"""dashguard.py : headset-side keeper for `frame-ctl dash hide` (blocker B4).

The dash-hide hook lives in the Frame Steam's SharedJSContext, so a Steam restart on the headset drops it and Steam
re-opens its dashboard, which takes all input from games. While ~/.local/state/framectl/dash-hide exists (frame-ctl
`dash hide` creates it, `dash allow` and `cleanup` remove it), this checks the hook every 3 s over Steam's local CEF
debug port and re-installs it, then hides the dashboard, whenever it's missing. Stdlib only (no websocket-client on
SteamOS). Runs as the user unit framectl-dashguard.service; log: journalctl --user -u framectl-dashguard.
"""
import base64, json, os, socket, struct, sys, time, urllib.request

FLAG = os.path.expanduser("~/.local/state/framectl/dash-hide")
CEF = "127.0.0.1:8080"
# Same hook as frame-ctl's DASH_JS with __noShowMain=true; keep the two in sync.
HOOK = """(()=>{if(!window.__fcDrop){window.__fcDrop=1;const o=WebSocket.prototype.send;WebSocket.prototype.send=function(d){
try{if(window.__noShowMain&&typeof d!="string"){const s=new TextDecoder("latin1").decode(d instanceof ArrayBuffer?new Uint8Array(d):d);
if(s.includes("ShowOverlay#1")&&s.includes("valve.steam.gamepadui.main")){(window.__fcDropped=window.__fcDropped||[]).push(Date.now());return}}}
catch(e){}return o.call(this,d)}};window.__noShowMain=true;return "ok"})()"""
CHECK = "window.__fcDrop===1&&window.__noShowMain===true"


def log(*a):
    print(time.strftime("%F %T"), *a, flush=True)


def ws_eval(url, js):
    """Runtime.evaluate over a bare-bones websocket (one text frame out, frames in until our reply)."""
    hostport, path = url.split("://", 1)[1].split("/", 1)
    h, p = hostport.split(":")
    s = socket.create_connection((h, int(p)), timeout=10)
    try:
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall(f"GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            c = s.recv(4096)
            if not c: raise OSError("closed during handshake")
            buf += c
        if b" 101 " not in buf.split(b"\r\n", 1)[0]: raise OSError("no websocket upgrade")
        buf = buf.split(b"\r\n\r\n", 1)[1]
        msg = json.dumps({"id": 1, "method": "Runtime.evaluate",
                          "params": {"expression": js, "returnByValue": True}}).encode()
        mask = os.urandom(4)
        n = len(msg)
        hdr = b"\x81" + (bytes([0x80 | n]) if n < 126 else b"\xfe" + struct.pack(">H", n) if n < 65536
                         else b"\xff" + struct.pack(">Q", n))
        s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(msg)))

        def need(k):
            nonlocal buf
            while len(buf) < k:
                c = s.recv(65536)
                if not c: raise OSError("closed")
                buf += c
        while True:
            need(2); ln = buf[1] & 0x7f; off = 2
            if ln == 126: need(4); ln = struct.unpack(">H", buf[2:4])[0]; off = 4
            elif ln == 127: need(10); ln = struct.unpack(">Q", buf[2:10])[0]; off = 10
            need(off + ln); data, buf = buf[off:off + ln], buf[off + ln:]
            try: r = json.loads(data)
            except ValueError: continue
            if r.get("id") == 1:
                return r.get("result", {}).get("result", {}).get("value")
    finally:
        s.close()


def page(title):
    pages = json.load(urllib.request.urlopen(f"http://{CEF}/json", timeout=5))
    return next((p["webSocketDebuggerUrl"] for p in pages if title in p["title"]), None)


def main():
    log("started; flag", FLAG)
    last = None
    while True:
        time.sleep(3)
        if not os.path.exists(FLAG):
            last = None; continue
        try:
            url = page("SharedJSContext")
            if not url: continue  # Steam not up yet
            if ws_eval(url, CHECK) is True:
                last = "ok"; continue
            ws_eval(url, HOOK)
            ok = ws_eval(url, CHECK) is True
            ws_eval(url, "SteamClient.OpenVR.VROverlay.HideDashboard()")
            log("hook was missing (Steam restarted?); re-installed:", ok, "and hid the dashboard")
            last = "fixed"
        except Exception as e:  # Steam restarting, port briefly gone: try again next round
            if last != "err": log("check failed:", repr(e)[:200])
            last = "err"


if __name__ == "__main__":
    sys.exit(main())
