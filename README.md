# Newcolor 7000 on 64-bit Windows

Lets Heidelberg **Newcolor 7000 2.0** drive its scanners on 64-bit Windows 10/11,
and scan files larger than 2 GB. It replaces one file, `HDSTI.dll`.

- Scanner works with **no Heidelberg driver** installed
- Scans above 2 GB and above 4 GB complete and save
- Tested with TOPAZ 2+ and TANGO on Windows 11 (Adaptec AVA-2906) and Windows 10,
  Newcolor 2.0.12 and 2.0.19

**[Download the latest release](../../releases/latest)**

---

## You need

- A SCSI card with a 64-bit Windows driver
- Your own licensed copy of Newcolor 7000 2.0 and its serial number

## Install

1. **Install Newcolor** to `C:\newcolor`, **not** Program Files.
   Use the `setup.exe` inside the disc's **`Setup`** folder — the one at the disc
   root won't start on 64-bit Windows.
   Don't install the HDHLusd driver.
2. **Close Newcolor**, then double-click **`patch\Install.cmd`** and accept the
   admin prompt. It finds Newcolor automatically and backs up the original DLL.
3. **Switch the scanner on, then start the PC.**
4. Run Newcolor **as administrator** and pick your scanner under *Input source*.

Tip: right-click the Newcolor shortcut → Properties → Compatibility → tick
*Run this program as an administrator*.

`patch\Uninstall.cmd` restores the original.

## Scans larger than 4 GB

Open `C:\newcolor\hdsti.ini` and set:

```
BigTIFFOutput=1
```

Files that large have to be saved as **BigTIFF**. Photoshop and Affinity open
them; GIMP crashes on very large images because its TIFF plug-in is 32-bit.
Leave it at `0` otherwise. Keep free disk space of about 2.5× the scan size.

*BigTIFF output is verified on Newcolor 2.0.12. On 2.0.19 it is untested with
this build — try a small scan first.*

## If it doesn't work

| Problem | Fix |
|---|---|
| *Could not find scanner* | Run Newcolor as administrator. Scanner must be on before the PC boots. |
| Still not found | Run `tools\scsiscan32.exe` as administrator. If the scanner isn't listed, check cable, termination and SCSI ID. If it says `CLAIMED`, a driver has taken it. |
| Scanner shows under *Other devices* with a yellow mark | That's correct — leave it. |
| Hangs on the splash screen | Windows print spooler is stuck. Test with `Get-Printer` in PowerShell; if that hangs too, run `Stop-Service Spooler -Force; Set-Service Spooler -StartupType Disabled` (turns off printing). |
| Licence errors that look like a bad serial | Newcolor is under Program Files. Reinstall to `C:\newcolor`. |
| *Not enough space* past 2 GB with plenty free | Patch isn't active. `C:\newcolor\HDSTI.dll` should be **65,536 bytes**. |

Something else: set `Log=1` in `hdsti.ini`, reproduce, and check `hdsti.log`
and `temphook.log` in `C:\newcolor`. Set it back to `0` after.

---

## Contents

```
patch/    HDSTI.dll, hdsti.ini, Install.cmd, Uninstall.cmd
tools/    scsiscan (SCSI device lister), BigTIFF and large-TIFF utilities
source/   full source
docs/     technical write-up
```

How it works: [`docs/TECHNICAL.md`](docs/TECHNICAL.md).

## Credits and licence

Thanks to Karl Hudson and Philipp Wagner.

The patch and its source are original work containing no Heidelberg code, released
under the MIT licence. **Newcolor 7000 is © Heidelberger Druckmaschinen AG and is
not included.** Not affiliated with Heidelberg. No warranty.
