"""Generates app.ico: blue rounded square + white eject symbol (triangle + bar)."""
import struct

def render(size, s):
    """Returns top-down rows of (b,g,r,a) tuples."""
    def inside_bg(x, y):
        m = 0.05
        r = 0.20
        x0, y0, x1, y1 = m, m, 1 - m, 1 - m
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        dx = x - cx
        dy = y - cy
        return dx * dx + dy * dy <= r * r

    def in_tri(x, y):
        ax, ay = 0.5, 0.16
        bx, by = 0.24, 0.50
        cx, cy = 0.76, 0.50
        d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by)
        d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy)
        d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)

    def in_bar(x, y):
        return 0.26 <= x <= 0.74 and 0.60 <= y <= 0.70

    BG = (11, 107, 184)   # blue
    FG = (255, 255, 255)  # white
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            ab = ag = ar = aa = 0
            for sy in range(s):
                for sx in range(s):
                    x = (px + (sx + 0.5) / s) / size
                    y = (py + (sy + 0.5) / s) / size
                    if inside_bg(x, y):
                        ab += BG[0]; ag += BG[1]; ar += BG[2]; aa += 255
                        if in_tri(x, y) or in_bar(x, y):
                            ab = ab - BG[0] + FG[0]
                            ag = ag - BG[1] + FG[1]
                            ar = ar - BG[2] + FG[2]
            cnt = s * s
            row.append((int(ab / cnt), int(ag / cnt), int(ar / cnt), int(aa / cnt)))
        rows.append(row)
    return rows

def ico_blob(rows, size):
    w = h = size
    xor = bytearray()
    for row in reversed(rows):          # bottom-up
        for (b, g, r, a) in row:
            xor += struct.pack('<BBBB', b, g, r, a)
    and_stride = ((w + 31) // 32) * 4
    andmask = bytes(and_stride * h)     # all zero; 32bpp alpha does the work
    hdr = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0,
                      len(xor) + len(andmask), 0, 0, 0, 0)
    return hdr + bytes(xor) + andmask

def main():
    sizes = [16, 24, 32, 48, 64, 256]
    blobs = []
    for s in sizes:
        rows = render(s, 4 if s <= 64 else 1)
        blobs.append((s, ico_blob(rows, s)))
    out = struct.pack('<HHH', 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries = b''
    data = b''
    for s, blob in blobs:
        w = 0 if s >= 256 else s
        entries += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(blob), offset)
        data += blob
        offset += len(blob)
    with open('app.ico', 'wb') as f:
        f.write(out + entries + data)
    print('app.ico written,', len(out + entries + data), 'bytes')

if __name__ == '__main__':
    main()
