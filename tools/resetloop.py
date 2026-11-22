#!/usr/bin/env python3
"""Reset the panel N times via RTS and classify each boot from the serial log.

OK    = reached 'web UI running' and no reset within the window
RESET = a reset/panic line appeared after app start (watchdog, panic, brownout)
HANG  = never reached 'web UI running'
Usage: resetloop.py N [window_s] [port]   (each boot's log goes to $TMPDIR/resetloop_<i>.log)
"""
import os, re, sys, tempfile, time, serial
n = int(sys.argv[1]); win = float(sys.argv[2]) if len(sys.argv) > 2 else 10; port = sys.argv[3] if len(sys.argv) > 3 else '/dev/ttyACM0'

def opener():
    for _ in range(500):
        try:
            s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.05
            s.dtr = False; s.rts = False; s.open(); return s
        except Exception:
            time.sleep(0.01)
    raise SystemExit('cannot open ' + port)

res = {}
for i in range(n):
    s = opener(); s.rts = True; time.sleep(0.1); s.rts = False; s.close()
    buf = b''; s = None; end = time.time() + win
    while time.time() < end:
        try:
            if s is None: s = opener()
            buf += s.read(4096)
        except Exception:
            s = None; time.sleep(0.01)
    if s: s.close()
    t = buf.decode('latin1')
    app = t.find('Calling app_main')
    after = t[app:] if app >= 0 else ''
    resets = re.findall(r'rst:0x[0-9a-f]+ \([A-Z0-9_]+\)', after)
    if resets: v = 'RESET ' + resets[0]
    elif 'web UI running' in t: v = 'OK'
    else: v = 'HANG'
    last = [l for l in t.splitlines() if re.match(r'[IWE] \(', l)]
    print(f'boot {i}: {v:40s} last: {last[-1][:70] if last else "-"}', flush=True)
    k = v.split()[0]; res[k] = res.get(k, 0) + 1
    open(os.path.join(tempfile.gettempdir(), f'resetloop_{i}.log'), 'w').write(t)
print(res)
