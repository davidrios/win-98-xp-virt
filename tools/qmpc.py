#!/usr/bin/env python3
"""Tiny QMP client: qmpc.py <socket> <cmd> [args...]
  screendump <out.png>         -> PPM via QMP, converted to PNG (pure python)
  keys <k1> <k2> ...           -> send-key one at a time (QKeyCode names; 'a+b' = chord)
  type <text>                  -> types ASCII text (letters, digits, \\ : . / space _ - & ( ) , ; = ' \" * % + ! > < | ~ ` ^ @ # $ [ ] { } ?; US layout)
  click <x> <y> [w h]          -> absolute pointer (usb-tablet) to x,y of a w×h screen (default 640×480), left click
  json <json>                  -> raw request
"""
import json, socket, struct, sys, time, zlib

def connect(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    f = s.makefile("rwb", buffering=0)
    greet = json.loads(f.readline())
    assert "QMP" in greet
    return f

def cmd(f, execute, arguments=None):
    req = {"execute": execute}
    if arguments:
        req["arguments"] = arguments
    f.write((json.dumps(req) + "\n").encode())
    while True:
        line = json.loads(f.readline())
        if "event" in line:
            continue
        return line

def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    # P6\nW H\n255\n
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", parts[0]
    w, h = map(int, parts[1].split())
    raw = parts[3]
    rows = b"".join(b"\x00" + raw[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b"")
    open(png, "wb").write(out)
    return w, h

KEYMAP = {" ": "spc", "\\": "backslash", ":": ("shift", "semicolon"), ".": "dot",
          "_": ("shift", "minus"), "-": "minus", "/": "slash", "\n": "ret",
          "&": ("shift", "7"), "(": ("shift", "9"), ")": ("shift", "0"), ",": "comma",
          ";": "semicolon", "=": "equal", "'": "apostrophe", '"': ("shift", "apostrophe"),
          "*": ("shift", "8"), "%": ("shift", "5"), "+": ("shift", "equal"), "!": ("shift", "1"),
          ">": ("shift", "dot"), "<": ("shift", "comma"), "|": ("shift", "backslash"),
          "~": ("shift", "grave_accent"), "`": "grave_accent", "^": ("shift", "6"), "@": ("shift", "2"),
          "#": ("shift", "3"), "$": ("shift", "4"), "[": "bracket_left", "]": "bracket_right",
          "{": ("shift", "bracket_left"), "}": ("shift", "bracket_right"), "?": ("shift", "slash")}
# US-International guests (the Brazilian XP images): " ' ^ ~ ` are dead keys
# there; a following space yields the character itself, so type '~ 1' for
# '~1' only on a US layout -- prefer 8.3 names with ~ (dead key + digit = both).

def send(f, names, hold=60):
    keys = [{"type": "qcode", "data": n} for n in names]
    r = cmd(f, "send-key", {"keys": keys, "hold-time": hold})
    if "error" in r:
        print("send-key", names, r["error"])
    time.sleep(0.12)

def main():
    path, what = sys.argv[1], sys.argv[2]
    f = connect(path)
    cmd(f, "qmp_capabilities")
    if what == "screendump":
        ppm = sys.argv[3] + ".ppm"
        r = cmd(f, "screendump", {"filename": ppm})
        if "error" in r:
            print(r); return 1
        time.sleep(0.3)
        print("screendump %dx%d -> %s" % (*ppm_to_png(ppm, sys.argv[3]), sys.argv[3]))
    elif what == "keys":
        for k in sys.argv[3:]:
            send(f, k.split("+"))
    elif what == "type":
        for ch in sys.argv[3]:
            k = KEYMAP.get(ch)
            if k is None:
                k = ("shift", ch.lower()) if ch.isupper() else ch
            send(f, list(k) if isinstance(k, tuple) else [k])
    elif what == "click":
        x, y = int(sys.argv[3]), int(sys.argv[4])
        w, h = (int(sys.argv[5]), int(sys.argv[6])) if len(sys.argv) > 6 else (640, 480)
        move = [{"type": "abs", "data": {"axis": "x", "value": x * 32767 // w}},
                {"type": "abs", "data": {"axis": "y", "value": y * 32767 // h}}]
        cmd(f, "input-send-event", {"events": move})
        time.sleep(0.15)
        for down in (True, False):
            r = cmd(f, "input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})
            if "error" in r:
                print("click", r["error"])
            time.sleep(0.1)
    elif what == "json":
        r = cmd(f, **json.loads(sys.argv[3]))
        print(json.dumps(r))
    return 0

if __name__ == "__main__":
    sys.exit(main())
