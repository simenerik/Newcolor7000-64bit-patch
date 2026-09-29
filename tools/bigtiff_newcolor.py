#!/usr/bin/env python3
"""
bigtiff_newcolor.py - streaming BigTIFF writer, Newcolor 7000 build

  bigtiff_newcolor.py   <- THIS FILE. Heidelberg Topaz / Tango / Primescan /
                           Nexscan via Newcolor 7000. Handles 8- or 16-bit,
                           3 or 4 samples, RGB / CIELAB / CMYK, preserves the
                           ICC profile. Also still handles ScanMate input.
  bigtiff.py            <- the original ScanMate 11000 / ColorQuartet version,
                           hardcoded to 3 samples x 16 bits.

Use this one for anything that came out of Newcolor.

ColorQuartet writes classic TIFF, whose file offsets are 32-bit. That caps any
output at 4 GB no matter how the executable is patched, and in practice it dies
earlier. BigTIFF uses 64-bit offsets and has no practical size limit.

This tool never holds a whole image in memory. It copies strip by strip, so a
60 GB output costs the same RAM as a 60 MB one.

WHAT IT HANDLES
---------------
* finalised ColorQuartet TIFFs (IFD present, samples little-endian)
* crashed / headless ColorQuartet output (IFD offset 0, samples big-endian --
  CQscan streams big-endian and byte-swaps during finalisation, so a scan that
  died before finalising is left unswapped)
* raw sample dumps, given --width
* Newcolor 7000 TIFFs from Heidelberg Topaz / Tango / Primescan / Nexscan,
  8- or 16-bit, RGB / CIELAB / CMYK, with the ICC profile carried across

Sample byte order is detected empirically rather than trusted from the header,
because CQscan's header says "II" even while the samples are big-endian.

COMMANDS
--------
    bigtiff_newcolor.py info    IN [--width N] [--spp N] [--bps N]
    bigtiff_newcolor.py convert IN -o OUT.btf [--width N] [--spp N] [--bps N] [--dpi D]
    bigtiff_newcolor.py stitch  IN1 IN2 ... -o OUT.btf [--width N] [--dpi D]
    bigtiff_newcolor.py verify  OUT.btf
    bigtiff_newcolor.py selftest

NEWCOLOR TEMP FILES
-------------------
Newcolor captures to _finescan.tmp in its Temporary Files folder before you
save. That file is raw headless pixel data and can be converted directly,
which is how a scan is recovered when Newcolor errors at finalisation:

    bigtiff_newcolor.py info  D:/_finescan.tmp

Newcolor holds the temp file with an exclusive lock while the error dialog is
up. Dismiss the dialog first, then copy the file out before closing Newcolor -
it deletes temp files on exit.

A Newcolor temp file is itself a valid classic TIFF with a working IFD, so no
--width/--spp/--bps is needed; everything is read from the file. Observed on a
2.6 GB capture: 3 x 16-bit CIELAB, 64 rows per strip, IFD intact well past the
2 GB mark.

Such a file can be larger than the image it describes. One capture carried
about a third more bytes than its strips accounted for - roughly one extra
channel's worth - apparently internal scratch beyond the three Lab channels.
The strips are what matter; the surplus is ignored.

PIXEL FORMAT
------------
For a finalised TIFF everything is read from the file - samples per pixel, bits
per sample, photometric interpretation and the ICC profile are all preserved.

For headless input the format cannot be known, so it must be supplied:

    --spp 3 --bps 16     ScanMate / ColorQuartet (the default)
    --spp 3 --bps 8      Newcolor 8-bit
    --spp 4 --bps 8      Newcolor CMYK

Newcolor records CIELAB (photometric 8) with an ICC profile. Both are carried
through unchanged; --photometric overrides it for headless input.

THE FULL-FRAME WORKFLOW
----------------------
Scan the whole frame in ONE pass. ColorQuartet writes every pixel to disk --
its WriteFile calls pass lpOverlapped=NULL, so the kernel appends at a 64-bit
file position and size is not a constraint on the pixel stream. What breaks
past 2 GB is only the bookkeeping: SetFilePointer(h, 0, NULL, FILE_CURRENT)
returns a 32-bit position, so the strip offsets and IFD offset it records are
wrong, and classic TIFF could not express them past 4 GB anyway.

Newcolor 7000 behaves identically. IDHTiffOutput.idh calls WriteFile with
lpOverlapped=NULL, writes pixel data contiguously from offset 8, and appends
the IFD at the end. All three SetFilePointer sites pass lpDistanceToMoveHigh=0,
so positions are 32-bit. A verified sample: 2780x4096, 133 strips of 31 rows,
first strip at offset 8, IFD immediately after the last pixel byte.

So let ColorQuartet capture, then replace the metadata:

    bigtiff_newcolor.py convert Scan-NN.tif -o Scan-NN.btf --width 27084

Tested end to end on a 4.39 GB single-frame capture: 27,084 x 27,000, eleven
strip offsets past the 4 GB mark, rows byte-identical to source, read back by
tifffile with no warnings.

`stitch` remains available for joining separate captures, but it is not
required for large frames.
"""

import argparse
import os
import struct
import sys

# Defaults for headless input only. A finalised TIFF carries its own values and
# these are never consulted. ScanMate/ColorQuartet is 3x16; Newcolor may be
# 3x8, 3x16, or 4x8/4x16 for CMYK.
DEFAULT_SPP = 3
DEFAULT_BPS = 16
STRIP_TARGET = 8 << 20  # aim for ~8 MB strips

# photometric interpretation
PHOTO_NAMES = {0: 'WhiteIsZero', 1: 'BlackIsZero', 2: 'RGB', 3: 'Palette',
               5: 'CMYK', 6: 'YCbCr', 8: 'CIELAB', 9: 'ICCLab', 10: 'ITULab'}

TAG_ICC = 34675
# Tags copied through from a finalised source when present. Purely descriptive
# ones: geometry and layout are always recomputed.
CARRY_TAGS = (271, 272, 305, 306, 315, 33432, 34377, 700)
HDR = 16                # BigTIFF header size

# TIFF field types
SHORT, LONG, RATIONAL, LONG8 = 3, 4, 5, 16
TYPESIZE = {1: 1, 2: 1, SHORT: 2, LONG: 4, RATIONAL: 8, 7: 1,
            9: 4, 11: 4, 12: 8, LONG8: 8, 17: 8}


# ----------------------------------------------------------------- input side

class Source:
    """A readable image: classic TIFF, headless CQscan output, or raw samples."""

    def __init__(self, path, width=None, spp=None, bps=None,
                 photometric=None, height=None):
        self.path = path
        self.size = os.path.getsize(path)
        self._f = open(path, 'rb')
        head = self._f.read(16)
        if len(head) < 8:
            raise ValueError(f'{path}: too small to be an image')

        self.byte_order = head[:2]
        en = '<' if self.byte_order == b'II' else '>'
        magic = struct.unpack(en + 'H', head[2:4])[0]
        ifd_off = struct.unpack(en + 'I', head[4:8])[0] if magic == 42 else 0

        if magic == 42 and ifd_off > 0:
            self._from_ifd(en, ifd_off)
            self.kind = 'classic TIFF'
        else:
            if width is None:
                raise ValueError(
                    f'{path}: no usable IFD (offset {ifd_off}). '
                    f'This is unfinalised scanner output - pass --width '
                    f'(and --spp/--bps if not {DEFAULT_SPP}x{DEFAULT_BPS}).')
            self.width = width
            self.spp = spp or DEFAULT_SPP
            self.bps = bps or DEFAULT_BPS
            self.photometric = photometric if photometric is not None else 2
            self.icc = None
            self.carried = {}
            self.data_start = HDR // 2          # 8-byte stub header
            rb = width * self.bytes_per_px
            avail = self.size - self.data_start
            self.height = height if height else avail // rb
            self.strips = [(self.data_start + i * rb, rb)
                           for i in range(self.height)]
            self.rows_per_strip = 1
            self.kind = 'headless (unfinalised)'
        self.sample_order = self._detect_order()

    @property
    def bytes_per_px(self):
        return self.spp * self.bps // 8

    def format_str(self):
        p = PHOTO_NAMES.get(self.photometric, f'photometric {self.photometric}')
        icc = f', ICC {len(self.icc):,}B' if self.icc else ''
        return f'{self.spp}x{self.bps}-bit {p}{icc}'

    def _from_ifd(self, en, off):
        f = self._f
        f.seek(off)
        n = struct.unpack(en + 'H', f.read(2))[0]
        tags = {}
        for _ in range(n):
            tag, typ, cnt = struct.unpack(en + 'HHI', f.read(8))
            raw = f.read(4)
            tags[tag] = (typ, cnt, raw, en)
        self._tags = tags

        def scalar(tag, default=None):
            if tag not in tags:
                return default
            typ, cnt, raw, e = tags[tag]
            if typ == SHORT:
                return struct.unpack(e + 'H', raw[:2])[0]
            return struct.unpack(e + 'I', raw)[0]

        def array(tag):
            typ, cnt, raw, e = tags[tag]
            sz = 2 if typ == SHORT else 4
            fmt = 'H' if typ == SHORT else 'I'
            if cnt * sz <= 4:
                return list(struct.unpack(e + fmt * cnt, raw[:cnt * sz]))
            ptr = struct.unpack(e + 'I', raw)[0]
            here = f.tell()
            f.seek(ptr)
            vals = list(struct.unpack(e + fmt * cnt, f.read(cnt * sz)))
            f.seek(here)
            return vals

        self.width = scalar(256)
        self.height = scalar(257)
        self.spp = scalar(277, 3)
        bits = array(258) if 258 in tags else [DEFAULT_BPS] * self.spp
        if len(set(bits)) != 1:
            raise ValueError(f'{self.path}: mixed bit depths {bits} are not supported')
        self.bps = bits[0]
        if self.bps not in (8, 16):
            raise ValueError(f'{self.path}: {self.bps} bits/sample; only 8 and 16 '
                             f'are supported')
        self.photometric = scalar(262, 2)
        if scalar(259, 1) != 1:
            raise ValueError(f'{self.path}: compressed input is not supported')
        if scalar(284, 1) != 1:
            raise ValueError(f'{self.path}: planar (separated) input is not supported')

        # ICC profile and descriptive tags, carried through verbatim
        self.icc = self._blob(TAG_ICC)
        self.carried = {}
        for t in CARRY_TAGS:
            b = self._blob(t)
            if b is not None:
                self.carried[t] = (tags[t][0], tags[t][1], b)
        self.rows_per_strip = scalar(278, self.height)
        offs = array(273)
        cnts = array(279)
        if len(offs) != len(cnts):
            raise ValueError(f'{self.path}: {len(offs)} strip offsets vs {len(cnts)} counts')
        self.strips = list(zip(offs, cnts))
        self.data_start = min(offs)

    def _blob(self, tag):
        """Raw bytes of a tag's value, or None. Used for ICC and pass-through."""
        if tag not in self._tags:
            return None
        typ, cnt, raw, en = self._tags[tag]
        sz = TYPESIZE.get(typ, 1)
        total = cnt * sz
        if total <= 4:
            return raw[:total]
        ptr = struct.unpack(en + 'I', raw)[0]
        here = self._f.tell()
        self._f.seek(ptr)
        b = self._f.read(total)
        self._f.seek(here)
        return b

    def _detect_order(self):
        """CQscan's header lies about sample order, so decide from the data.

        Real image samples change slowly between neighbours; the byte-swapped
        reading of the same bytes jumps wildly. Mean absolute difference between
        consecutive samples separates the two cleanly."""
        if self.bps == 8:
            # single-byte samples have no byte order to detect
            self._order_evidence = None
            return 'little'
        off, cnt = self.strips[len(self.strips) // 2]
        self._f.seek(off)
        buf = self._f.read(min(cnt, 1 << 20))
        buf = buf[:len(buf) // 2 * 2]
        if len(buf) < 64:
            return 'little'
        n = len(buf) // 2
        le = struct.unpack(f'<{n}H', buf)
        be = struct.unpack(f'>{n}H', buf)

        def mad(v):
            step = self.spp
            pairs = range(0, len(v) - 1, step)
            k = max(1, len(v) // step)
            return sum(abs(v[i + 1] - v[i]) for i in pairs) / k

        dl, db = mad(le), mad(be)
        self._order_evidence = (dl, db)
        return 'little' if dl <= db else 'big'

    def expected_bytes(self):
        return self.width * self.height * self.bytes_per_px

    def actual_bytes(self):
        return sum(c for _, c in self.strips)

    def rows(self, chunk_rows):
        """Yield (nrows, bytes) in row-aligned chunks, streaming."""
        rb = self.width * self.bytes_per_px
        buf = bytearray()
        have = 0
        for off, cnt in self.strips:
            self._f.seek(off)
            remaining = cnt
            while remaining:
                take = min(remaining, 1 << 22)
                data = self._f.read(take)
                if not data:
                    break
                remaining -= len(data)
                buf.extend(data)
                while len(buf) >= rb * chunk_rows:
                    out = bytes(buf[:rb * chunk_rows])
                    del buf[:rb * chunk_rows]
                    have += chunk_rows
                    yield chunk_rows, out
        whole = len(buf) // rb
        if whole:
            yield whole, bytes(buf[:whole * rb])

    def close(self):
        self._f.close()


# ---------------------------------------------------------------- output side

class BigTiffWriter:
    """Writes a single-image BigTIFF: header, then strips, then the IFD."""

    def __init__(self, path, width, dpi=None, byte_order='<',
                 spp=DEFAULT_SPP, bps=DEFAULT_BPS, photometric=2,
                 icc=None, carried=None, resolution_unit=2):
        self.path = path
        self.width = width
        self.dpi = dpi
        self.en = byte_order
        self.spp = spp
        self.bps = bps
        self.photometric = photometric
        self.icc = icc
        self.carried = carried or {}
        self.resolution_unit = resolution_unit
        self.rows_written = 0
        self.strip_offsets = []
        self.strip_counts = []
        self._rb = width * spp * bps // 8
        self.rows_per_strip = max(1, STRIP_TARGET // self._rb)
        self._f = open(path, 'wb')
        self._f.write(b'\x00' * HDR)            # header patched at close()
        self._buf = bytearray()                 # rows not yet flushed to a strip

    def write_rows(self, nrows, data):
        expect = nrows * self._rb
        if len(data) != expect:
            raise ValueError(f'write_rows: got {len(data)} bytes, expected {expect}')
        self._buf.extend(data)
        self.rows_written += nrows
        self._flush(full_only=True)

    def _flush(self, full_only):
        """Emit strips. TIFF requires every strip but the last to hold exactly
        RowsPerStrip rows, so partial strips are only written at the very end."""
        span = self.rows_per_strip * self._rb
        while len(self._buf) >= span:
            self.strip_offsets.append(self._f.tell())
            self.strip_counts.append(span)
            self._f.write(bytes(self._buf[:span]))
            del self._buf[:span]
        if not full_only and self._buf:
            self.strip_offsets.append(self._f.tell())
            self.strip_counts.append(len(self._buf))
            self._f.write(bytes(self._buf))
            self._buf.clear()

    def close(self):
        self._flush(full_only=False)
        f = self._f
        en = self.en
        H = self.rows_written
        n = len(self.strip_offsets)
        if n == 0:
            raise ValueError('no image data was written')

        # ---- IFD entries, tags MUST be ascending ----
        # each entry is (tag, type, count, inline_bytes_or_None, payload_or_None)
        entries = []

        def add(tag, typ, count, payload):
            if len(payload) <= 8:
                entries.append((tag, typ, count, payload.ljust(8, b'\x00'), None))
            else:
                entries.append((tag, typ, count, None, payload))

        add(254, LONG, 1, struct.pack(en + 'I', 0))
        add(256, LONG, 1, struct.pack(en + 'I', self.width))
        add(257, LONG, 1, struct.pack(en + 'I', H))
        sp = self.spp
        add(258, SHORT, sp, struct.pack(en + 'H' * sp, *([self.bps] * sp)))
        add(259, SHORT, 1, struct.pack(en + 'H', 1))          # no compression
        add(262, SHORT, 1, struct.pack(en + 'H', self.photometric))
        add(273, LONG8, n, struct.pack(en + f'{n}Q', *self.strip_offsets))
        add(277, SHORT, 1, struct.pack(en + 'H', sp))
        add(278, LONG, 1, struct.pack(en + 'I', self.rows_per_strip))
        add(279, LONG8, n, struct.pack(en + f'{n}Q', *self.strip_counts))
        if self.dpi:
            add(282, RATIONAL, 1, struct.pack(en + 'II', int(self.dpi), 1))
            add(283, RATIONAL, 1, struct.pack(en + 'II', int(self.dpi), 1))
        add(284, SHORT, 1, struct.pack(en + 'H', 1))          # chunky
        if self.dpi:
            add(296, SHORT, 1, struct.pack(en + 'H', self.resolution_unit))
        # CIELAB uses signed a*/b*; everything else here is unsigned
        fmt = 2 if self.photometric in (8, 9, 10) else 1
        add(339, SHORT, sp, struct.pack(en + 'H' * sp, *([fmt] * sp)))
        # descriptive tags carried from the source, then the ICC profile
        for tag, (typ, cnt, payload) in sorted(self.carried.items()):
            if tag in (t[0] for t in entries):
                continue
            add(tag, typ, cnt, payload)
        if self.icc:
            add(TAG_ICC, 7, len(self.icc), self.icc)

        entries.sort(key=lambda e: e[0])
        tags_seen = [e[0] for e in entries]
        if tags_seen != sorted(set(tags_seen)):
            raise RuntimeError('IFD tags must be unique and ascending')

        ifd_off = f.tell()
        blob_off = ifd_off + 8 + 20 * len(entries) + 8
        resolved, blobs, cursor = [], [], blob_off
        for tag, typ, count, inline, payload in entries:
            if inline is not None:
                resolved.append((tag, typ, count, inline))
            else:
                if len(payload) != count * TYPESIZE[typ]:
                    raise RuntimeError(f'tag {tag}: payload {len(payload)} != '
                                       f'{count} x {TYPESIZE[typ]}')
                resolved.append((tag, typ, count, struct.pack(en + 'Q', cursor)))
                blobs.append(payload)
                cursor += len(payload)

        f.write(struct.pack(en + 'Q', len(resolved)))
        for tag, typ, count, val in resolved:
            f.write(struct.pack(en + 'HHQ', tag, typ, count) + val)
        f.write(struct.pack(en + 'Q', 0))       # no next IFD
        for b in blobs:
            f.write(b)

        f.seek(0)
        f.write((b'II' if en == '<' else b'MM')
                + struct.pack(en + 'HHH', 43, 8, 0)
                + struct.pack(en + 'Q', ifd_off))
        f.close()
        return H, n


# ------------------------------------------------------------------- reading

def read_bigtiff(path):
    f = open(path, 'rb')
    head = f.read(16)
    en = '<' if head[:2] == b'II' else '>'
    magic, offsize, pad = struct.unpack(en + 'HHH', head[2:8])
    if magic != 43:
        raise ValueError(f'{path}: magic {magic}, not BigTIFF (43)')
    if offsize != 8:
        raise ValueError(f'{path}: offset size {offsize}, expected 8')
    ifd = struct.unpack(en + 'Q', head[8:16])[0]
    f.seek(ifd)
    n = struct.unpack(en + 'Q', f.read(8))[0]
    tags = {}
    for _ in range(n):
        tag, typ, cnt = struct.unpack(en + 'HHQ', f.read(12))
        val = f.read(8)
        tags[tag] = (typ, cnt, val)

    def get(tag):
        typ, cnt, val = tags[tag]
        sz = TYPESIZE[typ]
        fmt = {SHORT: 'H', LONG: 'I', LONG8: 'Q', RATIONAL: 'II'}[typ]
        total = cnt * sz
        if total <= 8:
            return list(struct.unpack(en + fmt * cnt, val[:total]))
        ptr = struct.unpack(en + 'Q', val)[0]
        here = f.tell()
        f.seek(ptr)
        out = list(struct.unpack(en + fmt * cnt, f.read(total)))
        f.seek(here)
        return out

    info = dict(path=path, byte_order=head[:2].decode(), ifd_offset=ifd,
                entries=n, width=get(256)[0], height=get(257)[0],
                bits=get(258), spp=get(277)[0],
                photometric=(get(262)[0] if 262 in tags else 2),
                has_icc=(TAG_ICC in tags),
                rows_per_strip=get(278)[0],
                strip_offsets=get(273), strip_counts=get(279),
                size=os.path.getsize(path))
    info['bytes_per_px'] = info['spp'] * info['bits'][0] // 8
    f.close()
    return info


# ------------------------------------------------------------------ commands

def cmd_info(a):
    for p in a.inputs:
        try:
            i = read_bigtiff(p)
            print(f'{p}: BigTIFF {i["width"]:,} x {i["height"]:,}, '
                  f'{i["spp"]}x{i["bits"][0]}-bit '
                  f'{PHOTO_NAMES.get(i["photometric"], i["photometric"])}, '
                  f'{len(i["strip_offsets"])} strips, {i["size"]:,} bytes')
            continue
        except Exception:
            pass
        s = Source(p, a.width, spp=getattr(a, 'spp', None), bps=getattr(a, 'bps', None),
                   photometric=getattr(a, 'photometric', None),
                   height=getattr(a, 'height', None))
        print(f'{p}')
        print(f'  kind          {s.kind}')
        print(f'  header        {s.byte_order.decode()}   samples {s.sample_order}-endian')
        print(f'  dimensions    {s.width:,} x {s.height:,}')
        print(f'  format        {s.format_str()}')
        print(f'  strips        {len(s.strips)}, rows/strip {s.rows_per_strip}')
        print(f'  pixel bytes   {s.actual_bytes():,} of {s.expected_bytes():,} expected'
              f'  {"COMPLETE" if s.actual_bytes() >= s.expected_bytes() else "SHORT"}')
        print(f'  file size     {s.size:,}')
        if s.kind.startswith('headless'):
            tail = s.size - s.data_start - s.height * s.width * s.bytes_per_px
            if tail:
                print(f'  trailing      {tail:,} bytes after the last whole row '
                      f'(partial row and/or broken metadata) - ignored')
        s.close()


def _build(out, sources, dpi, quiet=False):
    width = sources[0].width
    for s in sources:
        if s.width != width:
            raise SystemExit(f'ERROR: width mismatch - {sources[0].path} is {width:,} px '
                             f'but {s.path} is {s.width:,} px')
    ref = sources[0]
    for s in sources[1:]:
        if (s.spp, s.bps) != (ref.spp, ref.bps):
            raise SystemExit(f'ERROR: pixel format mismatch - {ref.path} is '
                             f'{ref.spp}x{ref.bps}-bit but {s.path} is '
                             f'{s.spp}x{s.bps}-bit')
        if s.photometric != ref.photometric:
            raise SystemExit(f'ERROR: colour space mismatch - {ref.path} is '
                             f'{PHOTO_NAMES.get(ref.photometric)} but {s.path} is '
                             f'{PHOTO_NAMES.get(s.photometric)}')
    orders = {s.sample_order for s in sources}
    if len(orders) > 1:
        raise SystemExit(f'ERROR: inputs disagree on sample byte order: '
                         + ', '.join(f'{s.path}={s.sample_order}' for s in sources))
    en = '<' if orders.pop() == 'little' else '>'

    w = BigTiffWriter(out, width, dpi=dpi, byte_order=en,
                      spp=ref.spp, bps=ref.bps, photometric=ref.photometric,
                      icc=ref.icc, carried=ref.carried)
    chunk = max(1, STRIP_TARGET // (width * ref.bytes_per_px))
    total = 0
    for s in sources:
        for nrows, data in s.rows(chunk):
            w.write_rows(nrows, data)
            total += nrows
            if not quiet and total % (chunk * 8) < chunk:
                print(f'\r  {total:,} rows', end='', flush=True)
    H, nstrips = w.close()
    if not quiet:
        print(f'\r  {H:,} rows, {nstrips} strips written')
    return H, nstrips, en


def cmd_convert(a):
    s = Source(a.inputs[0], a.width, spp=a.spp, bps=a.bps,
               photometric=a.photometric, height=a.height)
    print(f'input : {s.path}  ({s.kind}, samples {s.sample_order}-endian)')
    print(f'        {s.width:,} x {s.height:,}, {s.format_str()}')
    H, n, en = _build(a.out, [s], a.dpi)
    s.close()
    _report(a.out, H, n)


def cmd_stitch(a):
    srcs = [Source(p, a.width) for p in a.inputs]
    print(f'stitching {len(srcs)} inputs, top to bottom:')
    for s in srcs:
        print(f'  {s.path:40} {s.width:,} x {s.height:,}  ({s.sample_order}-endian)')
    total_h = sum(s.height for s in srcs)
    print(f'output height will be {total_h:,} rows')
    H, n, en = _build(a.out, srcs, a.dpi)
    for s in srcs:
        s.close()
    if H != total_h:
        raise SystemExit(f'ERROR: wrote {H:,} rows, expected {total_h:,}')
    _report(a.out, H, n)


def _report(path, H, n):
    i = read_bigtiff(path)
    ok = (i['height'] == H and len(i['strip_offsets']) == n)
    declared = sum(i['strip_counts'])
    expect = i['width'] * i['height'] * i['bytes_per_px']
    print(f'\noutput: {path}')
    print(f'  {i["width"]:,} x {i["height"]:,}, {len(i["strip_offsets"])} strips')
    print(f'  {i["spp"]}x{i["bits"][0]}-bit '
          f'{PHOTO_NAMES.get(i["photometric"], i["photometric"])}'
          f'{", ICC preserved" if i["has_icc"] else ""}')
    print(f'  pixel bytes {declared:,} of {expect:,} expected'
          f'  {"OK" if declared == expect else "MISMATCH"}')
    print(f'  file size {i["size"]:,} ({i["size"]/1e9:.2f} GB)')
    print(f'  readback  {"OK" if ok and declared == expect else "FAILED"}')
    if not (ok and declared == expect):
        raise SystemExit(1)


def cmd_verify(a):
    for p in a.inputs:
        i = read_bigtiff(p)
        declared = sum(i['strip_counts'])
        expect = i['width'] * i['height'] * i['bytes_per_px']
        bpp = i['bytes_per_px']
        last = max(o + c for o, c in zip(i['strip_offsets'], i['strip_counts']))
        checks = [
            ('magic 43 / offset size 8', True),
            ('IFD within file', i['ifd_offset'] < i['size']),
            ('strip data within file', last <= i['size']),
            ('pixel bytes match dimensions', declared == expect),
            ('bit depth uniform and 8 or 16',
             len(set(i['bits'])) == 1 and i['bits'][0] in (8, 16)),
            ('bits array length matches samples per pixel',
             len(i['bits']) == i['spp']),
            ('strips non-overlapping and ascending',
             all(i['strip_offsets'][k] + i['strip_counts'][k] <= i['strip_offsets'][k + 1]
                 for k in range(len(i['strip_offsets']) - 1))),
            ('strip count matches ceil(height / rows_per_strip)',
             len(i['strip_offsets']) ==
             -(-i['height'] // i['rows_per_strip'])),
            ('all strips but the last are exactly rows_per_strip rows',
             all(c == i['rows_per_strip'] * i['width'] * bpp
                 for c in i['strip_counts'][:-1])),
        ]
        print(f'{p}: {i["width"]:,} x {i["height"]:,}, '
              f'{i["spp"]}x{i["bits"][0]}-bit '
              f'{PHOTO_NAMES.get(i["photometric"], i["photometric"])}, '
              f'{i["size"]:,} bytes')
        good = True
        for name, c in checks:
            good &= c
            print(f'   {"PASS" if c else "FAIL"}  {name}')
        print(f'   -> {"VALID" if good else "INVALID"}')
        if not good:
            raise SystemExit(1)


def cmd_selftest(a):
    import tempfile, hashlib, random
    ok = True

    def chk(c, m):
        nonlocal ok
        ok &= c
        print(f'  {"PASS" if c else "FAIL"}  {m}')

    tmp = tempfile.mkdtemp()
    W, H1, H2 = 1000, 37, 51
    random.seed(7)

    def make_classic(path, w, h, en, seed):
        """A minimal but valid classic TIFF, one strip per row."""
        rng = random.Random(seed)
        # image-like: smooth gradients plus a little noise, as a real scan looks
        rows = []
        for y in range(h):
            vals = []
            for x in range(w):
                base = (x * 37 + y * 91 + seed * 1000) % 40000 + 10000
                for c in range(DEFAULT_SPP):
                    vals.append(min(65535, max(0, base + c * 700 + rng.randrange(-60, 60))))
            rows.append(struct.pack(en + f'{w*DEFAULT_SPP}H', *vals))
        data = b''.join(rows)
        rb = w * DEFAULT_SPP * DEFAULT_BPS // 8
        f = open(path, 'wb')
        f.write(b'II' + struct.pack('<HI', 42, 0))
        f.write(data)
        ifd = f.tell()
        offs = [8 + k * rb for k in range(h)]
        cnts = [rb] * h
        ent = [(256, LONG, 1, struct.pack('<I', w)),
               (257, LONG, 1, struct.pack('<I', h)),
               (258, SHORT, 3, None), (259, SHORT, 1, struct.pack('<HH', 1, 0)),
               (262, SHORT, 1, struct.pack('<HH', 2, 0)),
               (273, LONG, h, None), (277, SHORT, 1, struct.pack('<HH', 3, 0)),
               (278, LONG, 1, struct.pack('<I', 1)), (279, LONG, h, None),
               (284, SHORT, 1, struct.pack('<HH', 1, 0))]
        blob_at = ifd + 2 + 12 * len(ent) + 4
        bps_off = blob_at
        off_off = bps_off + 6
        cnt_off = off_off + 4 * h
        f.seek(ifd)
        f.write(struct.pack('<H', len(ent)))
        for tag, typ, cnt, val in ent:
            if val is None:
                ptr = {258: bps_off, 273: off_off, 279: cnt_off}[tag]
                val = struct.pack('<I', ptr)
            f.write(struct.pack('<HHI', tag, typ, cnt) + val)
        f.write(struct.pack('<I', 0))
        f.write(struct.pack('<HHH', 16, 16, 16))
        f.write(struct.pack(f'<{h}I', *offs))
        f.write(struct.pack(f'<{h}I', *cnts))
        f.seek(4)
        f.write(struct.pack('<I', ifd))      # patch the real IFD offset
        f.close()
        return data

    print('selftest: single-file convert')
    a_path = os.path.join(tmp, 'a.tif')
    d1 = make_classic(a_path, W, H1, '<', 1)
    s = Source(a_path)
    chk(s.width == W and s.height == H1, f'source parsed {s.width}x{s.height}')
    chk(s.sample_order == 'little', f'sample order detected: {s.sample_order}')
    out = os.path.join(tmp, 'a.btf')
    Hh, n, en = _build(out, [s], None, quiet=True)
    s.close()
    i = read_bigtiff(out)
    chk(i['width'] == W and i['height'] == H1, 'BigTIFF dimensions match')
    body = b''.join(open(out, 'rb').read()[o:o + c]
                    for o, c in zip(i['strip_offsets'], i['strip_counts']))
    chk(hashlib.md5(body).hexdigest() == hashlib.md5(d1).hexdigest(),
        'pixel data byte-identical after conversion')

    print('selftest: stitch')
    b_path = os.path.join(tmp, 'b.tif')
    d2 = make_classic(b_path, W, H2, '<', 2)
    sa, sb = Source(a_path), Source(b_path)
    out2 = os.path.join(tmp, 'ab.btf')
    Hh, n, en = _build(out2, [sa, sb], None, quiet=True)
    sa.close(); sb.close()
    i2 = read_bigtiff(out2)
    chk(i2['height'] == H1 + H2, f'stitched height {i2["height"]} = {H1}+{H2}')
    body2 = b''.join(open(out2, 'rb').read()[o:o + c]
                     for o, c in zip(i2['strip_offsets'], i2['strip_counts']))
    chk(hashlib.md5(body2).hexdigest() == hashlib.md5(d1 + d2).hexdigest(),
        'stitched pixel data byte-identical and in order')

    print('selftest: headless input')
    hl = os.path.join(tmp, 'headless.raw')
    with open(hl, 'wb') as f:
        f.write(b'II' + struct.pack('<HI', 42, 0))
        f.write(d1)
    s = Source(hl, width=W)
    chk(s.height == H1, f'headless height inferred: {s.height}')
    chk(s.kind.startswith('headless'), 'recognised as unfinalised')
    s.close()

    print('selftest: big-endian samples')
    be = os.path.join(tmp, 'be.tif')
    dbe = make_classic(be, W, H1, '>', 3)
    s = Source(be)
    chk(s.sample_order == 'big', f'detected big-endian samples: {s.sample_order}')
    s.close()

    print('selftest: rejects mismatched widths')
    c_path = os.path.join(tmp, 'c.tif')
    make_classic(c_path, W + 10, 5, '<', 4)
    sa, sc = Source(a_path), Source(c_path)
    try:
        _build(os.path.join(tmp, 'bad.btf'), [sa, sc], None, quiet=True)
        chk(False, 'width mismatch rejected')
    except SystemExit:
        chk(True, 'width mismatch rejected')
    sa.close(); sc.close()

    print('selftest: verify catches a truncated file')
    trunc = os.path.join(tmp, 'trunc.btf')
    raw = open(out, 'rb').read()
    open(trunc, 'wb').write(raw[:len(raw) - 200] + raw[-200:][:100])
    try:
        cmd_verify(argparse.Namespace(inputs=[trunc]))
        chk(False, 'truncated file rejected')
    except SystemExit:
        chk(True, 'truncated file rejected')
    except Exception:
        chk(True, 'truncated file rejected')

    print('selftest: 8-bit CIELAB round trip (Newcolor layout)')
    nc = os.path.join(tmp, 'nc8.tif')
    W8, H8, SPP8, RPS = 640, 200, 3, 31
    rb8 = W8 * SPP8
    px = bytes((x * 3 + y * 7 + c * 11) & 0xFF
               for y in range(H8) for x in range(W8) for c in range(SPP8))
    with open(nc, 'wb') as f:
        f.write(b'II' + struct.pack('<HI', 42, 0))
        f.write(px)
        ifd = f.tell()
        nst = -(-H8 // RPS)
        offs, cnts, o = [], [], 8
        for k in range(nst):
            rows_here = min(RPS, H8 - k * RPS)
            offs.append(o); cnts.append(rows_here * rb8); o += rows_here * rb8
        icc_blob = bytes(range(256)) * 2
        ent = [(256, LONG, 1), (257, LONG, 1), (258, SHORT, 3), (259, SHORT, 1),
               (262, SHORT, 1), (271, 2, 12), (273, LONG, nst), (277, SHORT, 1),
               (278, LONG, 1), (279, LONG, nst), (284, SHORT, 1), (34675, 7, len(icc_blob))]
        blob = ifd + 2 + 12 * len(ent) + 4
        bo, oo, co, mo, io = blob, blob + 6, blob + 6 + 4 * nst, \
                             blob + 6 + 8 * nst, blob + 6 + 8 * nst + 12
        f.seek(ifd); f.write(struct.pack('<H', len(ent)))
        vals = {256: struct.pack('<I', W8), 257: struct.pack('<I', H8),
                258: struct.pack('<I', bo), 259: struct.pack('<HH', 1, 0),
                262: struct.pack('<HH', 8, 0), 271: struct.pack('<I', mo),
                273: struct.pack('<I', oo), 277: struct.pack('<HH', SPP8, 0),
                278: struct.pack('<I', RPS), 279: struct.pack('<I', co),
                284: struct.pack('<HH', 1, 0), 34675: struct.pack('<I', io)}
        for tag, typ, cnt in ent:
            f.write(struct.pack('<HHI', tag, typ, cnt) + vals[tag])
        f.write(struct.pack('<I', 0))
        f.write(struct.pack('<HHH', 8, 8, 8))
        f.write(struct.pack(f'<{nst}I', *offs))
        f.write(struct.pack(f'<{nst}I', *cnts))
        f.write(b'NewcolorV2\x00\x00')
        f.write(icc_blob)
        f.seek(4); f.write(struct.pack('<I', ifd))

    s8 = Source(nc)
    chk(s8.bps == 8 and s8.spp == 3, f'read 8-bit 3-sample: {s8.spp}x{s8.bps}')
    chk(s8.photometric == 8, f'read CIELAB photometric: {s8.photometric}')
    chk(s8.icc is not None and len(s8.icc) == len(icc_blob),
        f'ICC profile read: {len(s8.icc) if s8.icc else 0} bytes')
    chk(271 in s8.carried, 'Make tag carried from source')
    o8 = os.path.join(tmp, 'nc8.btf')
    Hh, n, en = _build(o8, [s8], None, quiet=True)
    s8.close()
    i8 = read_bigtiff(o8)
    chk(i8['bits'] == [8, 8, 8], f'output bit depth {i8["bits"]}')
    chk(i8['photometric'] == 8, f'output photometric {i8["photometric"]}')
    chk(i8['has_icc'], 'ICC profile present in output')
    body8 = b''.join(open(o8, 'rb').read()[o:o + c]
                     for o, c in zip(i8['strip_offsets'], i8['strip_counts']))
    chk(hashlib.md5(body8).hexdigest() == hashlib.md5(px).hexdigest(),
        '8-bit pixel data byte-identical')

    print('selftest: headless 8-bit needs --spp/--bps')
    hl8 = os.path.join(tmp, 'headless8.raw')
    with open(hl8, 'wb') as f:
        f.write(b'II' + struct.pack('<HI', 42, 0)); f.write(px)
    s = Source(hl8, width=W8, spp=3, bps=8)
    chk(s.height == H8, f'headless 8-bit height inferred: {s.height}')
    chk(s.bytes_per_px == 3, f'bytes per pixel {s.bytes_per_px}')
    s.close()

    print()
    print('SELFTEST', 'PASSED' if ok else 'FAILED')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('info', 'convert', 'stitch', 'verify'):
        p = sub.add_parser(name)
        p.add_argument('inputs', nargs='+')
        p.add_argument('--width', type=int, help='pixel width, required for headless input')
        p.add_argument('--height', type=int,
                       help='row count, headless input only; default is inferred '
                            'from file size (which counts trailing junk as rows)')
        p.add_argument('--spp', type=int, choices=(1, 3, 4),
                       help=f'samples per pixel for headless input (default {DEFAULT_SPP}); '
                            f'3=RGB/Lab, 4=CMYK. Read from the file when finalised.')
        p.add_argument('--bps', type=int, choices=(8, 16),
                       help=f'bits per sample for headless input (default {DEFAULT_BPS}). '
                            f'Read from the file when finalised.')
        p.add_argument('--photometric', type=int,
                       help='photometric interpretation for headless input '
                            '(2=RGB, 5=CMYK, 8=CIELAB). Read from the file when finalised.')
        p.add_argument('--dpi', type=float, help='resolution to record in the output')
        if name in ('convert', 'stitch'):
            p.add_argument('-o', '--out', required=True)
    sub.add_parser('selftest')
    a = ap.parse_args()
    return {'info': cmd_info, 'convert': cmd_convert, 'stitch': cmd_stitch,
            'verify': cmd_verify, 'selftest': cmd_selftest}[a.cmd](a) or 0


if __name__ == '__main__':
    sys.exit(main())
