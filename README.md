# Newcolor 7000 on 64-bit Windows

Heidelberg's **Newcolor 7000 2.0** scanning software cannot drive its scanners on 64-bit
Windows. The scanner driver it depends on, `HDHLusd.dll`, is 32-bit, and Windows x64
refuses to install it:

> The folder you specified does not contain a compatible software driver for the device.
> If the folder contains a driver, make sure it is made to work with Windows for x64-based systems.

This patch replaces **one DLL** so Newcolor talks to the scanner directly over SCSI
pass-through, and that driver is no longer needed at all.

**Confirmed working on Windows 11 x64** with a Heidelberg **TOPAZ 2+** and a **TANGO**
on an Adaptec AVA-2906, running **Newcolor 7000 2.0.12**. Other 2.0.x builds should be
unaffected since this patch only touches one DLL unrelated to versioning, but only
2.0.12 has actually been tested.

---

## Installing Newcolor itself on 64-bit Windows

Before the scanner patch is even relevant, the Newcolor installer itself needs a
workaround on 64-bit Windows.

**The `setup.exe` at the root of the install disc will not run.** This is very likely
because it's a 16-bit stub whose only job was to detect the OS and hand off to the real
installer — normal for software this age. 64-bit Windows has no way to run 16-bit code
at all (unlike 32-bit programs, which run fine via WOW64), so that root launcher simply
fails outright.

**The fix:** don't run the top-level `setup.exe`. Instead, open the **`Setup`** folder on
the disc and run the setup `.exe` inside it directly. That one is the real 32-bit
InstallShield installer, and it runs under 64-bit Windows without any problem.

Install to somewhere **outside Program Files** — see the SCSI hardware notes below for
why — then proceed to the scanner patch.

---

## Install

1. Install Newcolor 7000 2.0 (see above for the 64-bit installer workaround), with your
   own serial number.
2. Download the release zip from the [Releases page](../../releases), extract it.
3. Close Newcolor.
4. Right-click **`Install.cmd`** → Run as administrator.
   (Double-clicking also works — it asks for elevation itself.)

The installer locates Newcolor automatically by checking, in order: the folder the patch
is run from, the Windows uninstall registry, a list of common install paths, and a search
of Program Files and the drive roots. If none of that finds it, it asks you for the path.

`Uninstall.cmd` puts the original back.

Then: **switch the scanner on before booting the PC** (SCSI is enumerated at startup),
and **run Newcolor as Administrator** (pass-through is refused to non-elevated programs).

**Do not install the HDHLusd driver.** It is 32-bit, Windows x64 will refuse it, and this
patch removes the need for it.

---

## Manual install (if `Install.cmd` doesn't work)

The installer is a convenience, not a requirement. Everything it does can be done by hand
in a few minutes, and this is exactly how the patch was first proven working, before any
installer existed.

1. Locate your Newcolor 7000 folder — the one containing `NC7000.exe`.

2. Open Command Prompt or PowerShell **as Administrator** in that folder.

3. Back up the original driver (skip this if you've already done it once):
   ```
   ren HDSTI.dll HDSTI_stock.dll
   ```

4. Copy in the replacement files from the downloaded release:
   ```
   copy "C:\path\to\downloaded\HDSTI.dll" .
   copy "C:\path\to\downloaded\hdsti.ini" .
   ```

5. Unblock the DLL, since Windows tags anything downloaded from the internet:
   ```
   Unblock-File HDSTI.dll
   ```

6. Run Newcolor **as Administrator** and select your scanner under Input source.

**To undo:** delete `HDSTI.dll`, then rename `HDSTI_stock.dll` back to `HDSTI.dll`.

If the scanner still isn't found after this, set `Log=1` in `hdsti.ini`, reproduce the
problem, and read `hdsti.log` in the same folder — see **Troubleshooting** below.

---

## Troubleshooting

Set `Log=1` in `hdsti.ini`, close and reopen Newcolor, reproduce the problem, then read
`hdsti.log` next to the DLL. Set it back to `0` afterward — logging every command slows
real scans noticeably.

| In the log | Meaning | What to do |
|---|---|---|
| `no scanner matching 'TOPAZ'` (or similar) | The scanner wasn't found on the bus, or its product string doesn't match what's configured | Check the `[types]` section in `hdsti.ini`. The log's `skipping '...' (does not match ...)` line shows the exact product string your scanner reported — use that. |
| `ioctl failed, win32 error 5` | Access denied | Run Newcolor as Administrator. SCSI pass-through is refused otherwise. |
| `scsi status 02  sense 05/20/00` | Illegal request / invalid command opcode | The scanner rejected `SendOpcode` or `ReceiveOpcode` in `hdsti.ini`. Try other values — see the comments in the file. |
| `scsi status 02  sense 05/24/00` | Invalid field in CDB | Opcode was accepted but something in the command layout was wrong. |
| `sense 06/29/00` | UNIT ATTENTION — the scanner just reset | Normal right after a firmware upload. The patch retries automatically once (`RetryUnitAttention=1` in v1.1+); on v1.0.x Newcolor itself handles the retry. |
| "Scanner X is already used as input source" | A `[types]` pattern in `hdsti.ini` is empty or too broad, so one scanner type is answering for another | Every entry under `[types]` must be a specific substring. Never leave one blank — an empty pattern matches *any* scanner. |
| Scanner not listed in Device Manager at all | Windows can't see it on the bus — this is a hardware problem, not a software one | Check: scanner switched **on before** the PC booted (SCSI is enumerated at startup only), bus terminated at both ends, no SCSI ID collision with the card (usually ID 7), cable seated properly. |
| Scanner shown under "Other devices" with a yellow warning icon | This is **correct and expected** | Windows has no built-in driver for SCSI processor-type devices, and none is wanted — the patch talks to it directly. |

**Before assuming a software problem, confirm Windows can see the scanner at all.** The
`tools/scsiscan` utility included in the release lists everything on the SCSI bus without
sending it any commands:

```
bus 0  id 5  lun 0  processor  LinoHell TOPAZ 2+ Scanner 1.0  free
```

`free` means nothing has claimed the device and pass-through can reach it. If the scanner
doesn't appear here, no amount of Newcolor or `hdsti.ini` configuration will fix it — see
`tools/README.md` for what to check.

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

If your scanner is not found, follow the **Troubleshooting** steps above — the `skipping`
line in `hdsti.log` tells you exactly what to put here.

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
