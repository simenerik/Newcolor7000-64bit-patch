# Newcolor 7000 on 64-bit Windows, with scans past 4 GB

Heidelberg's **Newcolor 7000 2.0** (2002) could not drive its drum scanners on
64-bit Windows, and could not produce a scan larger than about 2 GB on any
Windows at all. Both are fixed here by replacing a single DLL.

**Confirmed on Windows 11 x64**, Heidelberg **TOPAZ 2+** and **TANGO**, Adaptec
**AVA-2906**, Newcolor **2.0.12**:

* scanner reachable with no Heidelberg driver installed
* a **6 GB** temp file written, read back by Newcolor itself, and displayed
* a **4.87 GB BigTIFF** saved — 12,042 × 67,363, 811 megapixels, 16-bit CIELAB
* a 4.3 GB XPan scan at 11,000 dpi saved and **opened in Photoshop**

---

## The two problems

### 1. The scanner driver cannot exist on x64

`HDHLusd.dll` is a **user-mode still-image minidriver** — a 32-bit in-process
COM server. On x64 the imaging service runs as a 64-bit process, and a 32-bit
in-process COM server can never load into one. No signing option, compatibility
shim or registry key changes that, and porting it needs source nobody has.

Windows says so directly:

> The folder you specified does not contain a compatible software driver for the
> device. If the folder contains a driver, make sure it is made to work with
> Windows for x64-based systems.

The fix is to cut one layer higher:

```
Topaz.ext / Topaz2.ext
  └─> KSS32.dll
       └─> HDSTI.dll          <- the ONLY module that touches the still-image stack
            └─> sti.dll -> stisvc (64-bit)
                 └─> HDHLusd.dll   <- 32-bit, cannot load here
```

`KSS32.dll` uses exactly **eight** functions from `HDSTI.dll`. Reimplementing
those on SCSI pass-through removes `sti.dll`, `stisvc` and `HDHLusd.dll`
entirely. Nothing above `HDSTI.dll` changes.

### 2. Every offset in the file pipeline is 32-bit

Newcolor writes through **libtiff 3.x**, registering its own I/O callbacks via
`TIFFClientOpen`. On Windows those callbacks are 32-bit:

```c
SetFilePointer(h, distance, NULL, method)   /* distance read as SIGNED */
GetFileSize(h, NULL)                        /* low 32 bits only */
```

Two separate walls follow:

**2 GiB** — with `lpDistanceToMoveHigh = NULL` the distance is signed, so any
position above 2,147,483,647 is negative and the seek fails with
`ERROR_NEGATIVE_SEEK` (131). Writing pixels never seeks — `TIFFAppendToStrip`
only issues `SEEK_END, 0`, which is never negative — so a large scan captures
perfectly and then dies at close, when libtiff seeks to write the directory.
Newcolor reports *"not enough space"* with the disk nearly empty.

**4 GiB** — `toff_t` is `uint32`, so past 2^32 every offset libtiff records is
wrapped, including the one `TIFFWriteDirectory` seeks to. Left alone it writes
the directory **on top of pixel data** and reports success.

---

## How it is fixed

`HDSTI.dll` is replaced with one that does three things.

**SCSI transport.** Finds the scanner by INQUIRY product string and talks to it
with `IOCTL_SCSI_PASS_THROUGH_DIRECT` on `\\.\SCSIn:`. The scanner reports as a
SCSI **processor** device (type 03); SEND (`0x0A`) and RECEIVE (`0x08`) carry the
byte stream. Eight exports, matching the original by **ordinal** — `KSS32.dll`
imports by ordinal, not name.

**Seek correction (2–4 GiB).** Patches the import tables of `IDHTempOutput.idh`,
`IDHTempInput.idh` and `IDHTiffOutput.idh` so their `SetFilePointer` calls land
in our code, which reinterprets the offset as unsigned and seeks with a real
64-bit value. Below 2 GiB the original path is used unchanged, so ordinary scans
behave identically to stock Newcolor.

**Offset translation (past 4 GiB).** Each time `SEEK_END` yields a position at or
above 4 GiB, the `(wrapped uint32, true uint64)` pair is recorded. Any later seek
whose value matches a recorded one is redirected to the true position. Exact
matches only — never arithmetic guessing, which gets small legitimate offsets
such as the 4-byte header pointer wrong and destroys pixel data.

Newcolor **renames** the temp file between writing and reading it
(`_finescan.tmp` -> `TImage<id>.tmp`), so the table is held in memory and matched
by file size at open. Nothing is written to disk.

**BigTIFF output.** The saved file is converted to BigTIFF at close so other
software can open it past 4 GiB — 16-byte header, 20-byte IFD entries, 64-bit
offsets. libtiff's first 8-byte header write is padded to 16 so the pixel data
clears the space a BigTIFF header needs, and the directory is rebuilt with true
64-bit strip offsets at close.

This applies to the **saved file only**. The temp files stay classic TIFF with
wrapped offsets, because libtiff 3.x cannot *read* BigTIFF at all and Newcolor
reads its temp file back. The two paths are told apart by which module opened
the handle, identified from the caller's return address — the modules load only
256 KB apart, so their sizes are read from their own PE headers rather than
assumed.

---

## Install

Close Newcolor, then run `patch\Install.cmd` as administrator. `Uninstall.cmd`
restores the original.

Then:

* switch the scanner on **before** booting the PC — SCSI is enumerated at startup
* run Newcolor **as administrator** — pass-through is refused otherwise
* do **not** install the HDHLusd driver
* install Newcolor **outside Program Files** — it writes its licence file into
  its own folder, which Windows redirects there, producing licence errors that
  look like a bad serial

The disc's root `setup.exe` will not run on x64 (16-bit stub). Use the installer
inside the `Setup` folder.

---

## Configuration — `hdsti.ini`

| Key | Meaning |
|---|---|
| `Vendor` | INQUIRY vendor filter. Leave **empty** — the TOPAZ iX reports `HDPPKIEL` while TANGO and TOPAZ 2 report `LinoHell` |
| `SendOpcode` / `ReceiveOpcode` | SCSI opcodes, decimal. 10 = `0x0A` SEND, 8 = `0x08` RECEIVE |
| `TempHook` | `0` off, `1` log only, `2` seek fix (2–4 GiB), `3` + offset translation (past 4 GiB) |
| `TempHookThresholdMB` | Where the seek fix engages. `0` = 2048. Lower it to exercise the path on a small scan |
| `TempHookWrapAtMB` | Where translation starts recording. `0` = 4096. Lower it to test the machinery cheaply |
| `Log` | `1` writes `hdsti.log`. Leave `0` — it logs every SCSI command |
| `BigTIFFOutput` | `1` writes the saved file as BigTIFF. Needed above 4 GiB; harmless below it, but classic TIFF is more widely compatible |
| `[types]` | `3` = TOPAZ 2 / 2+ / iX, `4` = TANGO / Primescan / Nexscan. Matched against the INQUIRY product string. **Never leave a line empty** — an empty pattern matches any scanner |

---

## What works and what does not

| | Status |
|---|---|
| Scanner on 64-bit Windows | Confirmed, many scans |
| Scans up to 2 GB | Always worked |
| Scans 2–4 GB | **Fixed and confirmed** |
| Temp file past 4 GiB, read back by Newcolor | **Fixed and confirmed** — 4.88 GB |
| Saved TIFF up to 4 GiB | Works. Leave `BigTIFFOutput=0` for maximum compatibility |
| Saved TIFF past 4 GiB | **Works** with `BigTIFFOutput=1` — 4.87 GB written, opened in Photoshop |

Nothing here has a size ceiling any more. The largest verified scan is 811
megapixels at 16-bit CIELAB.

Note that **GIMP will crash** on anything much over 2 GB — its TIFF plug-in is a
separate 32-bit process and loads the whole image. That is a GIMP limitation,
not a problem with the file; the same plug-in opens a 22 MB BigTIFF fine.
Photoshop and Affinity are 64-bit and handle these.

---

## Tools

| | |
|---|---|
| `scsiscan32/64.exe` | Lists SCSI devices. Read-only. Run this before blaming the software |
| `seektest.ps1` | Proves the 2 GiB seek failure on your machine in about a second, using a 1-byte file |
| `tiffinfo.ps1` | TIFF header and tags without loading the file |
| `bigtiff_newcolor.py` | Converts a Newcolor TIFF to BigTIFF; recovers scans whose metadata is broken |
| `tifcrop.py` | Pulls a crop or thumbnail from a huge TIFF, reading only the strips it needs. Converts CIELAB to sRGB |
| `Set-LargeAddressAware.ps1` | Sets the 4 GB address-space flag. **Not needed** for anything here, and risky — 2001-era code with pointers above `0x80000000` corrupts silently |

---

## Hardware notes

* The **Adaptec AVA-2906 is an AIC-7850**, in the AIC-78xx family despite the
  model number. Works on Windows 11 x64 with the *modified* `AHA-29xx` `djsvs`
  package; the plain `Aic78xx` package does not recognise it.
* That package has **no `.cat` file** — editing the INF invalidated the WHQL
  catalog — so it needs Secure Boot off and test signing on.
* It is a **boot-start** driver. Take a restore point before installing it.
* The scanner appears under **Other devices** with a yellow mark. That is correct
  — Windows has no driver for SCSI processor devices and none is wanted.
* The Topaz refuses scan lengths above roughly 160–180 mm. Hardware limit.

---

## Building

32-bit only.

```sh
i686-w64-mingw32-windres version.rc -O coff -o version.o
i686-w64-mingw32-gcc -shared -o HDSTI.dll hdsti_spti.c temphook.c version.o \
    hdsti.def -Wall -Os -s -static-libgcc
```

The `.def` fixes the export **ordinals** — `KSS32.dll` imports by ordinal, so
they must never be reordered. The linker's stdcall-fixup warnings are expected;
the decorations it resolves to (`@4 @20 @4 @8 @4 @16 @16 @12`) match the
`ret imm16` values in the original binary.

`source/libtiff_sidecar_proof.c` builds against libtiff 3.9.7 and reproduces the
whole 4.6 GB round trip with no hardware. `source/hook_order_test.c` replays the
real seek sequence through the hook's decision order.

---

## How this was actually found

Worth recording, because reasoning was wrong repeatedly and testing was not.

**Build the falsifying test first.** `seektest.ps1` turned two days of inference
into fact in one second: `ERROR_NEGATIVE_SEEK` at every position above 2 GiB,
and every one of those positions succeeding with a real high dword.

**Make the expensive test cheap.** `TempHookWrapAtMB=16` forced the >4 GiB code
path onto a 20 MB scan and immediately exposed that Newcolor *renames* the temp
file between writing and reading it — which would have cost an hour to discover
on a real scan, and which nobody had predicted.

**Check the arithmetic, not the return code.** `TIFFWriteDirectory` returned
success while writing its directory on top of the image. The only symptom was a
file size exactly equal to header + pixels. Nothing failed; nothing was logged.

**Verify against the real library.** libtiff 3.9.7 built from source, driven
through the same callback design, reproduced each failure and then each fix
before any of it touched a scanner: a 4.6 GB round trip with 600 sample rows and
zero mismatches, and a 4.6 GB BigTIFF validated by `tifffile` with warnings
promoted to errors.

**Make the expensive test cheap, then run the expensive one anyway.** Lowering
the recording threshold to 16 MB exercised the whole >4 GiB path on a 20 MB scan
and immediately exposed that Newcolor *renames* the temp file between writing and
reading it — nobody had predicted that, and on a real scan it would have cost an
hour to find. But the cheap test could not exercise the large-number arithmetic,
so the 90-minute scan still had to happen.

Conclusions that were confidently wrong along the way: the SCSI card's chipset
family; which scanner type number meant which model; that the whole-file memory
mapping in `IDHTempInput` was a hard blocker (libtiff degrades gracefully when
the map proc returns 0); that the pixel path never seeks backward past 4 GiB
(the directory write does); that past 4 GiB libtiff writes no directory at all
(it writes one, on top of the image). Each was caught by a test, none by more
analysis.

Two bugs were caught by checking the built artefact rather than the source: a
translation step placed after the threshold check it needed to precede, so it
would silently never run; and a module-size default that would have made three
modules loading 256 KB apart indistinguishable. Both compiled cleanly and would
have failed silently.

---

## Licence

The replacement `HDSTI.dll` and its source are original work, written from
analysis of the interface between `KSS32.dll` and `HDSTI.dll`. They contain no
Heidelberg code and are released under the MIT licence.

**Newcolor 7000 itself is copyright Heidelberger Druckmaschinen AG and is not
included here.** Use your own licensed copy.

Not affiliated with or endorsed by Heidelberger Druckmaschinen AG. No warranty —
this drives expensive and irreplaceable hardware. Keep `HDSTI_stock.dll` so you
can always put things back.
