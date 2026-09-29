<#
    seektest.ps1 - does SetFilePointer with lpDistanceToMoveHigh = NULL fail
                   above 2 GiB on this machine?

    Seeking beyond end-of-file is legal in Win32, so this needs a 1-byte file
    and about a second. Each position is tried twice: once the way libtiff's
    Win32 seek proc does it (high = NULL), once with a real high dword.

    From the ColorQuartet method note, section 2. Run before patching anything.
#>

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Seek {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr CreateFileW(string p, uint acc, uint share,
        IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint SetFilePointer(IntPtr h, int lo, IntPtr hi, uint method);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern uint SetFilePointer(IntPtr h, int lo, ref int hi, uint method);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr h);
    public const uint GENERIC_WRITE = 0x40000000;
    public const uint CREATE_ALWAYS = 2;
    public const uint FILE_BEGIN = 0;
    public const uint INVALID = 0xFFFFFFFF;
}
'@

$tmp = Join-Path $env:TEMP 'seektest.bin'
[System.IO.File]::WriteAllBytes($tmp, @(0))
$h = [Seek]::CreateFileW($tmp, [Seek]::GENERIC_WRITE, 0, [IntPtr]::Zero,
                         [Seek]::CREATE_ALWAYS, 0x80, [IntPtr]::Zero)
if ($h -eq [IntPtr]::new(-1)) { throw "cannot open $tmp" }

$positions = @(
    @{ n = '2 147 483 647  (2 GiB - 1)'; v = 2147483647 },
    @{ n = '2 147 483 648  (2 GiB)'    ; v = 2147483648 },
    @{ n = '2 802 960 552  (your 2.6 GB temp)'; v = 2802960552 },
    @{ n = '3 221 225 472  (3 GiB)'    ; v = 3221225472 },
    @{ n = '4 294 967 295  (4 GiB - 1)'; v = 4294967295 }
)

Write-Host ''
Write-Host '  position                              high=NULL (as shipped)   high ptr (fixed)' -ForegroundColor White
Write-Host '  ---------------------------------------------------------------------------------'

foreach ($p in $positions) {
    $lo = [BitConverter]::ToInt32([BitConverter]::GetBytes([uint64]$p.v), 0)

    $r1 = [Seek]::SetFilePointer($h, $lo, [IntPtr]::Zero, [Seek]::FILE_BEGIN)
    $e1 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $s1 = if ($r1 -eq [Seek]::INVALID -and $e1 -ne 0) { "FAIL  err $e1" } else { 'OK' }

    $hi = [int]([uint64]$p.v -shr 32)
    $r2 = [Seek]::SetFilePointer($h, $lo, [ref]$hi, [Seek]::FILE_BEGIN)
    $e2 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $s2 = if ($r2 -eq [Seek]::INVALID -and $e2 -ne 0) { "FAIL  err $e2" } else { 'OK' }

    $c1 = if ($s1 -eq 'OK') { 'Green' } else { 'Red' }
    Write-Host ('  {0,-38}' -f $p.n) -NoNewline
    Write-Host ('{0,-25}' -f $s1) -ForegroundColor $c1 -NoNewline
    Write-Host $s2 -ForegroundColor Green
}

[void][Seek]::CloseHandle($h)
Remove-Item $tmp -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '  err 131 = ERROR_NEGATIVE_SEEK: the distance was read as a signed' -ForegroundColor Gray
Write-Host '  32-bit value and came out negative. That is the 2 GiB fault.' -ForegroundColor Gray
Write-Host ''
