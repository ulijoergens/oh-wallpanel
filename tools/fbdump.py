#!/usr/bin/env python3
"""Grab the panel's screen as a PNG via the serial console command 'fbdump'.

Usage: fbdump.py out.png [port]          (reads from the running panel, no reset)
       fbdump.py out.png --log file.txt  (decode a saved console log instead)

The panel prints base64 of run-length pairs (u16 count, u16 RGB565 pixel, little
endian) between FBDUMP-BEGIN <w> <h> and FBDUMP-END. Only the standard library is used.
"""
import base64, re, struct, sys, time, zlib


def grab(port):
    import serial
    s = serial.Serial(port, 115200, timeout=0.2)   # plain open: does not reset the board
    s.read(1 << 16)
    s.write(b'fbdump\r\n')
    buf, end = b'', time.time() + 60
    while time.time() < end and b'FBDUMP-END' not in buf:
        buf += s.read(1 << 16)
    s.close()
    return buf.decode('latin1')


def decode(text):
    lines = text.replace('\r', '').split('\n')
    start = next(i for i, l in enumerate(lines) if l.startswith('FBDUMP-BEGIN'))
    w, h = map(int, lines[start].split()[1:3])
    data = bytearray()
    for l in lines[start + 1:]:
        if l.startswith('FBDUMP-END'):
            break
        l = l.strip()
        if re.fullmatch(r'[A-Za-z0-9+/]+={0,2}', l) and len(l) % 4 == 0:   # skip stray log lines
            data += base64.b64decode(l)
    rgb = bytearray()
    for off in range(0, len(data) - 3, 4):
        run, px = struct.unpack_from('<HH', data, off)
        r = (px >> 11) & 31; g = (px >> 5) & 63; b = px & 31
        rgb += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * run
    if len(rgb) != w * h * 3:
        raise ValueError(f'got {len(rgb) // 3} pixels, expected {w * h}')
    return w, h, rgb


def png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    out = sys.argv[1]
    if len(sys.argv) > 3 and sys.argv[2] == '--log':
        text = open(sys.argv[3], encoding='latin1').read()
        w, h, rgb = decode(text)
    else:
        for attempt in range(3):   # the USB console occasionally loses a line
            try:
                w, h, rgb = decode(grab(sys.argv[2] if len(sys.argv) > 2 else '/dev/ttyACM0'))
                break
            except (ValueError, StopIteration) as e:
                print(f'attempt {attempt + 1}: {e}', file=sys.stderr)
        else:
            raise SystemExit('no clean dump after 3 attempts')
    png(out, w, h, rgb)
    print(f'{out}: {w}x{h}')
