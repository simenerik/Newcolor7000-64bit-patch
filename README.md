# Newcolor 7000 on 64-bit Windows

Heidelberg's **Newcolor 7000 2.0** scanning software cannot drive its scanners on 64-bit
Windows. The scanner driver it depends on, `HDHLusd.dll`, is 32-bit, and Windows x64
refuses to install it:

> The folder you specified does not contain a compatible software driver for the device.
> If the folder contains a driver, make sure it is made to work with Windows for x64-based systems.

This patch replaces **one DLL** so Newcolor talks to the scanner directly over SCSI
pass-through, and that driver is no longer needed at all.

**Confirmed working on Windows 11 x64** with a Heidelberg **TOPAZ 2+** and a **TANGO**
on an Adaptec AVA-2906.

---

## Install

1. Install Newcolor 7000 2.0 normally, with your own serial number.
2. Download the release zip, extract it.
3. Close Newcolor.
4. Right-click **`Install.cmd`** → Run as administrator.

`Uninstall.cmd` puts the original back.

Then: **switch the scanner on before booting the PC** (SCSI is enumerated at startup),
and **run Newcolor as Administrator** (pass-through is refused to non-elevated programs).

**Do not install the HDHLusd driver.** It is 32-bit, Windows x64 will refuse it, and this
patch removes the need for it.

---

## Why the driver cannot simply be ported

`HDHLusd.dll` is a **user-mode still-image minidriver** — a 32-bit in-process COM server.
On 64-bit Windows the imaging service runs as a 64-bit process, and a 32-bit in-process
COM server can *never* load into one. No signing option, compatibility shim or registry
key changes that. Porting it would need source that no longer exists publicly.

The fix is to cut one layer higher up:

```
Topaz.ext / Topaz2.ext
  └─> KSS32.dll
       └─> HDSTI.dll          ← the only module that touches the still-image stack
            └─> sti.dll → stisvc (64-bit)
                 └─> HDHLusd.dll   ← 32-bit, cannot load here
                      └─> SCSI hardware
```

`HDSTI.dll` is the sole module in the entire product that talks to the still-image stack,
and `KSS32.dll` uses just **eight** of its functions. Reimplementing those eight on top of
SCSI pass-through removes `sti.dll`, `stisvc` and `HDHLusd.dll` from the chain entirely.
Nothing above `HDSTI.dll` changes — `KSS32.dll` and the application never know.

The scanner reports as a SCSI **processor** device (type 03), whose command set is just
SEND (`0x0A`) and RECEIVE (`0x08`). The original minidriver built no CDBs of its own — it
opened the device like a file and did plain reads and writes — so the kernel was wrapping
the byte stream in exactly those two commands. That is what the replacement issues.

---

## Supported scanners

There are only two scanner modules, and both route through `KSS32.dll` → `HDSTI.dll`:

| Module | Type | Scanners |
|---|---|---|
| `Topaz2.ext` | 3 | TOPAZ 2, TOPAZ 2+, TOPAZ iX |
| `Topaz.ext` | 4 | TANGO, Primescan, Nexscan F4000 |

`HDSCSI.dll` ships with Newcolor and exports a full ASPI interface, but nothing imports
it. It is unused.

- **Confirmed on hardware:** TOPAZ 2+, TANGO
- **Same code path, untested:** TOPAZ 2, TOPAZ iX, Primescan, Nexscan F4000

For the untested four the transport is identical; the only thing that can differ is device
matching. `hdsti.ini` maps each type number to a substring matched against the scanner's
INQUIRY product string:

```ini
[types]
3=TOPAZ
4=TANGO
```

If your scanner is not found, set `Log=1`, reproduce, and look in `hdsti.log` for the
`skipping` line — it prints the exact string the scanner reported. Put that here.
**Never leave a line empty**: an empty pattern matches any scanner, so one type will claim
a device belonging to the other family and Newcolor reports
*"Scanner TANGO is already used as input source"*.

---

## SCSI hardware notes

Hard-won and not documented anywhere obvious:

- The **Adaptec AVA-2906 is an AIC-7850** — it belongs to the AIC-78xx family, despite the
  model number suggesting otherwise. It works on Windows 11 x64 with the *modified*
  `AHA-29xx` `djsvs` driver package. The plain `Aic78xx` package does not recognise it.
- That driver package has **no `.cat` file**, because editing the INF invalidated the
  original WHQL catalog. It is unsigned as far as driver installation is concerned, so it
  needs Secure Boot off and test signing on.
- It is a **boot-start** driver (`StartType=0`). If it misbehaves the machine may not boot.
  **Take a restore point before installing it.**
- Newcolor's own readme recommends the **AHA-29160N**, which has signed x64 drivers.
- Install Newcolor **outside Program Files**. It writes its licence file into its own
  folder; Windows redirects or blocks that under Program Files, producing licence errors
  that look like a bad serial number.

## Check the scanner before blaming the software

Prove Windows can see the scanner before involving Newcolor at all. The `scsiscan` tool
lists everything on the SCSI bus (read-only — it sends no commands to any device):

```
bus 0  id 5  lun 0  processor  LinoHell TOPAZ 2+ Scanner 1.0  free
```

`free` means nothing has claimed the device and pass-through can reach it. In Device
Manager the scanner appears under **Other devices** with a yellow mark — that is correct
and expected, since Windows has no driver for processor-type devices and none is wanted.

If it does not appear, the problem is below the software: power-on order, bus termination,
or a SCSI ID collision with the card (usually ID 7).

---

## Building

32-bit only — Newcolor is a 32-bit application.

```sh
i686-w64-mingw32-windres version.rc -O coff -o version.o
i686-w64-mingw32-gcc -shared -o HDSTI.dll hdsti_spti.c version.o hdsti.def \
    -Wall -Os -s -static-libgcc
```

The `.def` file fixes the export **ordinals**. `KSS32.dll` imports by ordinal, not by name,
so they must never be reordered. The linker's stdcall-fixup warnings are expected — the
decorations it resolves to (`@4 @20 @4 @8 @4 @16 @16 @12`) match the `ret imm16` values in
the original binary.

---

## Licence and credit

The replacement `HDSTI.dll` and its source are **original work**, written from analysis of
the interface between `KSS32.dll` and `HDSTI.dll`. They contain no Heidelberg code and are
released under the MIT licence.

**Newcolor 7000 itself is copyright Heidelberger Druckmaschinen AG and is not included
here.** Use your own licensed copy. This project does not bypass or modify its licensing.

Not affiliated with or endorsed by Heidelberger Druckmaschinen AG. No warranty — this
drives expensive and irreplaceable hardware. `Uninstall.cmd` and the `HDSTI_stock.dll`
backup are there so you can always put things back.
