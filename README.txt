Newcolor 7000 2.0 - 64-bit Windows scanner patch
================================================
Version 1.0

Lets Newcolor 7000 drive Heidelberg TANGO and TOPAZ drum scanners on 64-bit
Windows. Confirmed working on Windows 11 x64 with a TOPAZ 2+ and a TANGO on
an Adaptec AVA-2906.


WHAT YOU NEED
-------------
  * Newcolor 7000 2.0 already installed, with your own serial number.
    This patch does NOT include Newcolor and does not bypass licensing.
  * A SCSI card with a working 64-bit driver.
  * The scanner modules present (Plug-Ins\Topaz.ext / Topaz2.ext and the
    Scanners\ folders). A demo-only install has nothing to patch.

Install Newcolor somewhere OUTSIDE Program Files, e.g. C:\newcolor.
It writes its licence file into its own folder, which Windows blocks or
silently redirects under Program Files, producing licence errors that look
like a bad serial.


INSTALL
-------
  1. Close Newcolor.
  2. Right-click Install.cmd -> Run as administrator.

It finds the installation, renames the original HDSTI.dll to
HDSTI_stock.dll, and puts the replacement in place. Run Uninstall.cmd to
put everything back.


THEN
----
  * Switch the scanner ON BEFORE booting the PC. SCSI is enumerated at
    startup; a device powered up later will not appear.
  * Run Newcolor AS ADMINISTRATOR. SCSI pass-through is refused to
    non-elevated programs.
  * Do NOT install the HDHLusd driver. It is a 32-bit still-image
    minidriver, 64-bit Windows will refuse it, and this patch removes the
    need for it.


WHY THIS EXISTS
---------------
Newcolor reaches the scanner like this:

    Topaz.ext / Topaz2.ext -> KSS32.dll -> HDSTI.dll -> Windows still-image
                                                        stack -> HDHLusd.dll

HDHLusd.dll is a 32-bit in-process COM server. On 64-bit Windows the imaging
service runs as a 64-bit process, and a 32-bit in-process server can never
load into one. No signing option, compatibility flag or registry key changes
that. Porting it would need source nobody has.

HDSTI.dll is the only module in the entire product that touches the
still-image stack, and KSS32.dll uses just eight of its functions. Replacing
it with SCSI pass-through removes the still-image stack and HDHLusd.dll from
the chain. Nothing above HDSTI.dll changes.


CONFIGURATION - hdsti.ini
-------------------------
Read at startup, in the same folder as the DLL.

  Vendor         Optional INQUIRY vendor filter. Leave EMPTY: the TOPAZ iX
                 reports "HDPPKIEL" while TANGO and TOPAZ 2 report
                 "LinoHell", so filtering here would exclude the iX.
  SendOpcode     SCSI SEND opcode, decimal. 10 = 0x0A.
  ReceiveOpcode  SCSI RECEIVE opcode, decimal. 8 = 0x08.
  TimeoutSec     Seconds per command. Firmware upload needs room.
  ChunkKB        Bytes per transfer, in KB. 0 = ask the adapter.
  Port           Restrict to one \\.\SCSIn: port. -1 searches all.
  Log            1 writes hdsti.log next to the DLL. Leave at 0 normally -
                 logging every command slows scans noticeably.
  OpenMode       2 = a repeated Open returns the same handle (default).
  ZeroOutArgs    Diagnostic only. Leave 0.

  [types]        Maps Newcolor's scanner-family number to a substring
                 matched against the INQUIRY product string, so the DLL
                 only answers for a scanner actually attached.
                   3 = TOPAZ   (TOPAZ 2, TOPAZ 2+, TOPAZ iX)
                   4 = TANGO


SUPPORTED
---------
  Confirmed:  TOPAZ 2+, TANGO
  Expected:   TOPAZ 2, TOPAZ iX   - same modules, same transport
  Unknown:    Primescan, Nexscan  - modules not available for testing.
              They may use HDSCSI.dll (a separate ASPI/SPTI path that ships
              with Newcolor) rather than HDSTI.dll, in which case this patch
              is not involved at all.


TROUBLESHOOTING
---------------
Set Log=1 in hdsti.ini, reproduce, then read hdsti.log.

  "no scanner matching 'TOPAZ'"
      The scanner is not on the bus, or reports a different product string.
      The log prints what it skipped and why. Adjust [types] to match.

  "ioctl failed, win32 error 5"
      Not elevated. Run Newcolor as Administrator.

  "scsi status 02  sense 05/20/00"
      Invalid opcode. Try other SendOpcode / ReceiveOpcode values.

  "sense 06/29/00"
      Unit attention / power-on reset. Normal right after a firmware
      upload; Newcolor retries by itself.

  Scanner not listed at all
      Confirm Windows can see it before involving Newcolor. It should
      appear in Device Manager under "Other devices" as a SCSI Processor
      Device with a yellow mark - that is correct and expected, since
      Windows has no driver for processor-type devices and none is wanted.
      Check power-on order, termination, and SCSI ID collisions.


BUILD FROM SOURCE
-----------------
See source\. Must be built 32-bit; Newcolor is a 32-bit application.

    i686-w64-mingw32-windres version.rc -O coff -o version.o
    i686-w64-mingw32-gcc -shared -o HDSTI.dll hdsti_spti.c version.o \
        hdsti.def -Wall -Os -s -static-libgcc

The .def file fixes the export ORDINALS. KSS32.dll imports by ordinal, not
by name, so they must never be reordered. The linker's stdcall-fixup
warnings are expected.


LICENCE AND CREDIT
------------------
The replacement HDSTI.dll and its source are original work, written from
analysis of the interface between KSS32.dll and HDSTI.dll. They contain no
Heidelberg code and may be copied, modified and redistributed freely.

Newcolor 7000 itself is copyright Heidelberger Druckmaschinen AG and is NOT
included here. Do not redistribute it. Use your own licensed copy.

Not affiliated with or endorsed by Heidelberger Druckmaschinen AG.
No warranty. This drives expensive hardware; keep HDSTI_stock.dll so you
can always put things back.
