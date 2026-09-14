# Newcolor 7000 on 64-bit Windows

Heidelberg's **Newcolor 7000 2.0** (2002) cannot drive its drum scanners on
64-bit Windows, and cannot produce a scan larger than about 2 GB on any Windows
at all. Both are fixed here by replacing a single DLL.

**Confirmed on Windows 11 x64** — Heidelberg **TOPAZ 2+** and **TANGO**, Adaptec
**AVA-2906**, Newcolor **2.0.12**:

- scanner reachable with no Heidelberg driver installed
- a **6 GB** working file written and read back by Newcolor itself
- a **4.87 GB BigTIFF** saved — 12,042 × 67,363, 811 megapixels, 16-bit CIELAB
- an XPan frame at 11,000 dpi, 14,069 × 55,082, saved at 4.3 GB and opened in Photoshop

**[Download the latest release](../../releases)** · [Installation guide](HOW-TO-INSTALL.txt)

---

## The two problems

### The scanner driver cannot exist on x64

`HDHLusd.dll` is a 32-bit **in-process COM server**. On x64 the imaging service
runs as a 64-bit process, and a 32-bit in-process COM server can never load into
one. No signing option, compatibility shim or registry key changes that, and
porting it needs source nobody has. Windows says so directly:

> The folder you specified does not contain a compatible software driver for the
> device. If the folder contains a driver, make sure it is made to work with
> Windows for x64-based systems.

The fix cuts one layer higher:

```
Topaz.ext / Topaz2.ext
  └─> KSS32.dll
       └─> HDSTI.dll          ← the ONLY module that touches the still-image stack
            └─> sti.dll → stisvc (64-bit)
                 └─> HDHLusd.dll   ← 32-bit, cannot load here
```

`KSS32.dll` uses exactly **eight** functions from `HDSTI.dll`. Reimplementing
those on SCSI pass-through removes `sti.dll`, `stisvc` and `HDHLusd.dll`
entirely. Nothing above `HDSTI.dll` changes.

### Every offset in the file pipeline is 32-bit

Newcolor writes through **libtiff 3.x**, registering its own I/O callbacks via
`TIFFClientOpen`. On Windows those callbacks are 32-bit, which produces two
separate walls.

**At 2 GiB** — with `lpDistanceToMoveHigh = NULL`, `SetFilePointer` reads the
distance as *signed*, so any position above 2,147,483,647 is negative and fails
with `ERROR_NEGATIVE_SEEK`. Writing pixels never seeks, so a large scan captures
perfectly and then dies at close when libtiff seeks to write the directory.
Newcolor reports *"not enough space"* with the disk nearly empty.

**At 4 GiB** — `toff_t` is `uint32`, so past 2³² every offset libtiff records is
wrapped, including the one it seeks to when writing the directory. Left alone it
writes the directory **on top of pixel data** and reports success.

---

## How it is fixed

**SCSI transport.** Finds the scanner by INQUIRY product string and talks to it
with `IOCTL_SCSI_PASS_THROUGH_DIRECT`. The scanner reports as a SCSI *processor*
device; SEND (`0x0A`) and RECEIVE (`0x08`) carry the byte stream.

**Seek correction.** Patches the import tables of `IDHTempOutput`, `IDHTempInput`
and `IDHTiffOutput` so their `SetFilePointer` calls reinterpret the offset as
unsigned and seek with a real 64-bit value. Below 2 GiB the original path is used
unchanged.

**Offset translation.** Each time `SEEK_END` yields a position at or above 4 GiB,
the `(wrapped, true)` pair is recorded; any later seek matching a recorded value
is redirected. Exact matches only — arithmetic un-wrapping guesses wrong on small
legitimate offsets like the 4-byte header pointer and destroys pixel data.
Newcolor *renames* the temp file between writing and reading it, so the table is
held in memory and matched by file size.

**BigTIFF output.** The saved file is converted at close — 16-byte header,
20-byte IFD entries, 64-bit offsets. The temp file deliberately stays classic
TIFF, because libtiff 3.x cannot *read* BigTIFF and Newcolor reads its temp back.

---

## Supported scanners

| Module | Type | Scanners |
|---|---|---|
| `Topaz2.ext` | 3 | TOPAZ 2, TOPAZ 2+, TOPAZ iX |
| `Topaz.ext` | 4 | TANGO, Primescan, Nexscan F4000 |

Confirmed on TOPAZ 2+ and TANGO. The rest share the same code path.

---

## SCSI hardware

Hard-won and not documented anywhere obvious:

- The **Adaptec AVA-2906 is an AIC-7850**, in the AIC-78xx family despite the
  model number. Works on Windows 11 x64 with the *modified* `AHA-29xx` `djsvs`
  package; the plain `Aic78xx` package does not recognise it.
- That package has **no `.cat` file**, so it needs Secure Boot off and test
  signing on. It is **boot-start** — take a restore point first.
- Most modern PCs are PCIe only, and PCIe SCSI cards are rare and expensive.
  An older PC with a PCI slot is often easier.
- The scanner appears under **Other devices** with a yellow mark. That is
  correct — Windows has no driver for SCSI processor devices and none is wanted.
- Install Newcolor **outside Program Files**; it writes its licence file into its
  own folder and Windows blocks that, producing errors that look like a bad serial.
- The disc's root `setup.exe` is 16-bit and will not run on x64. Use the one
  inside the `Setup` folder.

---

## Building

32-bit only.

```sh
i686-w64-mingw32-windres version.rc -O coff -o version.o
i686-w64-mingw32-gcc -shared -o HDSTI.dll hdsti_spti.c temphook.c version.o \
    hdsti.def -Wall -Os -s -static-libgcc
```

The `.def` fixes the export **ordinals** — `KSS32.dll` imports by ordinal, so
they must never be reordered.

`source/` includes three harnesses that reproduce everything against real
libtiff 3.9.7 with no hardware: a 4.6 GB round trip, a 4.6 GB BigTIFF validated
by `tifffile` with warnings as errors, and a replay of the hook's decision order.

---

## Credits

Thanks to **Karl Hudson** for keeping these machines alive, and to
**Philipp Wagner**, who solved the same problems independently and has related
work of his own.

---

## Licence

The replacement `HDSTI.dll` and its source are original work, written from
analysis of the interface between `KSS32.dll` and `HDSTI.dll`. They contain no
Heidelberg code and are released under the MIT licence.

**Newcolor 7000 itself is copyright Heidelberger Druckmaschinen AG and is not
included here.** Use your own licensed copy. This project does not bypass or
modify its licensing.

Not affiliated with or endorsed by Heidelberger Druckmaschinen AG. No warranty —
this drives expensive and irreplaceable hardware. `Uninstall.cmd` and the
`HDSTI_stock.dll` backup are there so you can always put things back.
