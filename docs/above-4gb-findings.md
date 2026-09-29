# Above 4 GiB with libtiff 3.x — verified against the real library

**11 September 2026.** Everything below was run against libtiff 3.9.7 built from
source — the same version Newcolor embeds — driven through `TIFFClientOpen` with
callbacks that reproduce the Win32 32-bit semantics. No hardware involved.

Source: `libtiff_sidecar_proof.c`

---

## Result

A **4.6 GB classic TIFF**, written and read back by libtiff 3.9.7, verified at
600 sample rows across the whole file with **zero mismatches**.

```
file size   4,599,983,324
pixels      4,599,983,190      64,955 rows x 70,818
header                   8
IFD                    126     2 + 10*12 + 4, at the true end
header diroff  305,015,902     wrapped, and translatable
```

**BigTIFF turned out not to be necessary.** The temp file stays a classic TIFF
with wrapped 32-bit offsets; a sidecar makes those offsets work on both sides.
That matters for Newcolor specifically, because libtiff 3.x cannot read BigTIFF
at all — and Newcolor reads its temp file back through `IDHTempInput`.

---

## What was wrong with the earlier reasoning

**"The pixel path is append-only, so nothing seeks backward past 4 GiB."**

True for the pixel phase — `TIFFAppendToStrip` only ever issues `SEEK_END, 0`,
and a zero distance is never negative. But it is **false for the directory
write**. `TIFFWriteDirectory` does a `SEEK_SET` to the position it recorded, and
past 4 GiB that value is wrapped.

Measured, with translation applied only on the read side:

```
TIFFWriteDirectory returned 1 (success)
file size 4,599,983,198   = header + pixels EXACTLY
at wrapped offset 305,015,902:  0a 00   <- entry count 10
```

libtiff reported success while writing its directory **on top of pixel data**,
~250 bytes destroyed mid-file, and `TIFFClose` discarded nothing because nothing
failed. A silent corruption that looks like success.

**"Past 4 GiB libtiff writes no directory at all."** Also wrong — it writes one,
just in the wrong place.

---

## The fix

Translate on **both** sides, from recorded truth rather than inference.

* Every time `SEEK_END` yields a position at or above 4 GiB, record the
  `(wrapped uint32, true uint64)` pair.
* On any `SEEK_SET`, look the value up. Where several true offsets share a
  wrapped value, take the one nearest the current position.
* Persist the table beside the file; the reader loads it.

Nothing is guessed. Un-wrapping by arithmetic — picking whichever multiple of
2^32 looks plausible — fails on small legitimate offsets such as the 4-byte
header pointer, and getting that wrong overwrites pixel data. An earlier
attempt did exactly that and produced a header `diroff` of 0.

Applying translation on write as well as read moved the IFD from mid-file to
the true end: the file grew by exactly 126 bytes, and the previously corrupted
region around offset 305,015,902 verified clean.

---

## Sidecar

572 bytes for a 4.6 GB file — 35 pairs.

```
wrapped      1,702,408  ->  true  4,296,669,704
wrapped     10,767,112  ->  true  4,305,734,408
wrapped     19,831,816  ->  true  4,314,799,112
```

---

## Still open for Newcolor

1. **Geometry.** In the harness the writer knows width/height/samples. Inside
   Newcolor the hook sees only bytes. Strip size gives `rowsPerStrip x rowBytes`,
   which does not separate width from samples and bit depth.
2. **Phase detection.** Write and read are separate handles in separate modules;
   the hook must know which sidecar belongs to which file and when to persist it.
3. **`IDHTempInput` maps the whole file.** `MapViewOfFile` with
   `dwNumberOfBytesToMap = 0` cannot map 4+ GB in a 32-bit process. libtiff
   degrades to normal I/O when the map proc returns 0, so this is survivable —
   and Heidelberg ships a no-mapping callback pair selected by a `'u'` in the
   mode string, at `0x1000AEF9` in `IDHTempInput.idh`.
4. **None of this has run inside Newcolor.** It is verified against real libtiff,
   which is not the same thing.
