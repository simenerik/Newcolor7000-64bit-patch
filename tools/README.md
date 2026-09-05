# scsiscan

Lists everything on the SCSI buses: vendor, product, revision, device type,
and whether Windows has already claimed the device.

**Read-only.** It sends no SCSI commands to any device — it only asks the port
driver for information it cached when it enumerated the bus at boot. Safe to
run with a drum scanner attached and powered.

## Run it

From an **Administrator** command prompt:

    scsiscan64.exe
    scsiscan32.exe

Both should show the same devices. Newcolor is 32-bit, so the 32-bit result is
the one that matters for it.

## Reading the output

    bus 0  id 5  lun 0  processor  LinoHell TOPAZ 2+ Scanner 1.0  free

- `processor` — SCSI device type 03, which is what these scanners report
- `free` — nothing has claimed it, so pass-through can reach it
- `** CLAIMED **` — a driver has taken it and pass-through will be blocked

The `max transfer` figure per port is what the patch chunks transfers to.

## If the scanner does not appear

The problem is below the software. In order of likelihood:

- The scanner must be **switched on before the PC boots** — SCSI is enumerated
  at startup, and a device powered up later will not appear.
- The bus must be terminated at both ends.
- No SCSI ID collision with the card (usually ID 7).
- Cable seated at both ends.

If no ports open at all, you are either not running as Administrator or the
SCSI card is not installed cleanly in Device Manager.

## Build

    i686-w64-mingw32-gcc   -o scsiscan32.exe scsiscan.c -Wall -Os -s
    x86_64-w64-mingw32-gcc -o scsiscan64.exe scsiscan.c -Wall -Os -s
