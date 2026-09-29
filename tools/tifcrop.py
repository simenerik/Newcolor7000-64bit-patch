#!/usr/bin/env python3
"""
tifcrop.py - pull a small crop out of a very large TIFF, without loading it

Reads classic TIFF and BigTIFF.

Reads only the strips the crop touches, so a 3.7 GB scan costs the same
memory as a thumbnail. Written to check that a large scan actually contains
a picture, when the file is too big for the usual viewers to open.

    python tifcrop.py scan.tif -o crop.png
    python tifcrop.py scan.tif -o crop.png --x 6000 --y 25000 --w 1200 --h 900
    python tifcrop.py scan.tif --thumb -o thumb.png

CIELAB input is converted to sRGB so the result looks right in any viewer.
Without Pillow the crop is written as a PPM, which most things still open.
"""

import argparse, struct, sys, os

TYPESIZE = {1:1, 2:1, 3:2, 4:4, 5:8, 6:1, 7:1, 8:2, 9:4, 10:8, 11:4, 12:8,
            16:8, 17:8, 18:8}


def read_ifd(f):
    """Returns (endian, tags, big). Handles classic TIFF and BigTIFF.

    BigTIFF differs in only three ways that matter here: a 16-byte header
    with a 64-bit IFD pointer, an 8-byte entry count, and 20-byte entries
    whose count and value fields are 8 bytes instead of 4."""
    f.seek(0)
    head = f.read(16)
    order = head[:2]
    if order == b'II':
        en = '<'
    elif order == b'MM':
        en = '>'
    else:
        sys.exit('not a TIFF')
    magic, = struct.unpack(en + 'H', head[2:4])

    if magic == 43:
        big = True
        offsize, = struct.unpack(en + 'H', head[4:6])
        if offsize != 8:
            sys.exit(f'BigTIFF with {offsize}-byte offsets is not supported')
        ifd, = struct.unpack(en + 'Q', head[8:16])
    elif magic == 42:
        big = False
        ifd, = struct.unpack(en + 'I', head[4:8])
    else:
        sys.exit(f'bad magic {magic}')

    f.seek(ifd)
    if big:
        n, = struct.unpack(en + 'Q', f.read(8))
    else:
        n, = struct.unpack(en + 'H', f.read(2))
    tags = {}
    for _ in range(n):
        if big:
            tag, typ, cnt = struct.unpack(en + 'HHQ', f.read(12))
            raw = f.read(8)
        else:
            tag, typ, cnt = struct.unpack(en + 'HHI', f.read(8))
            raw = f.read(4)
        tags[tag] = (typ, cnt, raw)
    return en, tags, big


def values(f, en, tags, tag, big=False):
    if tag not in tags:
        return None
    typ, cnt, raw = tags[tag]
    sz = TYPESIZE.get(typ, 1)
    total = cnt * sz
    inline = 8 if big else 4
    if total <= inline:
        data = raw[:total]
    else:
        off, = struct.unpack(en + ('Q' if big else 'I'), raw)
        here = f.tell()
        f.seek(off)
        data = f.read(total)
        f.seek(here)
    if typ == 3:
        return list(struct.unpack(en + f'{cnt}H', data))
    if typ in (4, 9):
        return list(struct.unpack(en + f'{cnt}I', data))
    if typ in (16, 17):                      # LONG8 / SLONG8, BigTIFF only
        return list(struct.unpack(en + f'{cnt}Q', data))
    if typ == 1:
        return list(data)
    return data


def lab_to_srgb(L, a, b):
    """CIELAB (D50) -> sRGB, 8-bit."""
    fy = (L + 16.0) / 116.0
    fx = fy + a / 500.0
    fz = fy - b / 200.0

    def finv(t):
        return t ** 3 if t > 6.0 / 29.0 else 3 * (6.0 / 29.0) ** 2 * (t - 4.0 / 29.0)

    # D50 white point
    X = 0.96422 * finv(fx)
    Y = 1.00000 * finv(fy)
    Z = 0.82521 * finv(fz)

    # D50 -> sRGB (Bradford-adapted)
    r =  3.1338561 * X - 1.6168667 * Y - 0.4906146 * Z
    g = -0.9787684 * X + 1.9161415 * Y + 0.0334540 * Z
    bb =  0.0719453 * X - 0.2289914 * Y + 1.4052427 * Z

    def gam(c):
        c = 0.0 if c < 0.0 else (1.0 if c > 1.0 else c)
        c = 1.055 * (c ** (1 / 2.4)) - 0.055 if c > 0.0031308 else 12.92 * c
        return int(c * 255 + 0.5)

    return gam(r), gam(g), gam(bb)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('-o', '--output', default='crop.png')
    ap.add_argument('--x', type=int, help='left edge (default: centre)')
    ap.add_argument('--y', type=int, help='top edge (default: centre)')
    ap.add_argument('--w', type=int, default=1200)
    ap.add_argument('--h', type=int, default=900)
    ap.add_argument('--thumb', action='store_true',
                    help='downsample the whole image instead of cropping')
    ap.add_argument('--thumb-width', type=int, default=1000)
    a = ap.parse_args()

    size = os.path.getsize(a.input)
    f = open(a.input, 'rb')
    en, tags, big = read_ifd(f)

    W = values(f, en, tags, 256, big)[0]
    H = values(f, en, tags, 257, big)[0]
    bits = values(f, en, tags, 258, big) or [8]
    spp = (values(f, en, tags, 277, big) or [1])[0]
    photo = (values(f, en, tags, 262, big) or [2])[0]
    comp = (values(f, en, tags, 259, big) or [1])[0]
    rps = (values(f, en, tags, 278, big) or [H])[0]
    offs = values(f, en, tags, 273, big)
    counts = values(f, en, tags, 279, big)

    bps = bits[0]
    if comp != 1:
        sys.exit(f'compressed input (compression {comp}) is not supported')
    if bps not in (8, 16):
        sys.exit(f'{bps} bits/sample not supported')

    bypp = spp * bps // 8
    rowbytes = W * bypp
    pname = {2: 'RGB', 5: 'CMYK', 8: 'CIELAB', 9: 'ICCLab'}.get(photo, str(photo))
    print(f'{a.input}')
    print(f'  {"BigTIFF" if big else "classic TIFF"}: {W:,} x {H:,}  '
          f'{spp}x{bps}-bit {pname}  {size:,} bytes')
    print(f'  {len(offs)} strips, {rps} rows/strip')

    def read_row(y):
        s = y // rps
        if s >= len(offs):
            return None
        within = y - s * rps
        f.seek(offs[s] + within * rowbytes)
        return f.read(rowbytes)

    if a.thumb:
        step = max(1, W // a.thumb_width)
        xs = list(range(0, W, step))
        ys = list(range(0, H, step))
        cw, ch = len(xs), len(ys)
        print(f'  thumbnail: every {step}th pixel -> {cw} x {ch}')
    else:
        x0 = a.x if a.x is not None else max(0, W // 2 - a.w // 2)
        y0 = a.y if a.y is not None else max(0, H // 2 - a.h // 2)
        cw = min(a.w, W - x0)
        ch = min(a.h, H - y0)
        xs = list(range(x0, x0 + cw))
        ys = list(range(y0, y0 + ch))
        print(f'  crop: {cw} x {ch} at ({x0:,}, {y0:,})')

    pix = bytearray(cw * ch * 3)
    fmt16 = en + 'H'
    o = 0
    for yi, y in enumerate(ys):
        row = read_row(y)
        if row is None or len(row) < rowbytes:
            break
        for x in xs:
            base = x * bypp
            if bps == 8:
                c = row[base:base + spp]
                if len(c) < spp:
                    break
                vals = list(c)
            else:
                vals = [struct.unpack_from(fmt16, row, base + k * 2)[0] for k in range(spp)]

            if photo in (8, 9):                      # CIELAB
                if bps == 8:
                    L = vals[0] * 100.0 / 255.0
                    A = vals[1] - 256 if vals[1] > 127 else vals[1]
                    B = vals[2] - 256 if vals[2] > 127 else vals[2]
                else:
                    L = vals[0] * 100.0 / 65535.0
                    A = (vals[1] - 65536 if vals[1] > 32767 else vals[1]) / 256.0
                    B = (vals[2] - 65536 if vals[2] > 32767 else vals[2]) / 256.0
                r, g, b = lab_to_srgb(L, A, B)
            elif photo == 5 and spp >= 4:            # CMYK
                if bps == 16:
                    vals = [v >> 8 for v in vals]
                c_, m_, y_, k_ = vals[:4]
                r = max(0, 255 - min(255, c_ + k_))
                g = max(0, 255 - min(255, m_ + k_))
                b = max(0, 255 - min(255, y_ + k_))
            else:                                    # RGB / grey
                if bps == 16:
                    vals = [v >> 8 for v in vals]
                if spp == 1:
                    r = g = b = vals[0]
                else:
                    r, g, b = vals[0], vals[1], vals[2]

            pix[o] = r; pix[o+1] = g; pix[o+2] = b
            o += 3
        if yi and yi % 200 == 0:
            print(f'    row {yi}/{ch}')

    f.close()

    try:
        from PIL import Image
        Image.frombytes('RGB', (cw, ch), bytes(pix)).save(a.output)
        print(f'  wrote {a.output}')
    except ImportError:
        out = os.path.splitext(a.output)[0] + '.ppm'
        with open(out, 'wb') as g:
            g.write(b'P6\n%d %d\n255\n' % (cw, ch))
            g.write(bytes(pix))
        print(f'  Pillow not installed - wrote {out} (most viewers open PPM)')
        print(f'  for PNG: pip install Pillow')


if __name__ == '__main__':
    main()
