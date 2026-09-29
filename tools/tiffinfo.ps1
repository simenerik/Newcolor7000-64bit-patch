<#
    tiffinfo.ps1 - print a TIFF's header and tag directory

    Reads only the header and the IFD, not the pixel data, so it is instant
    even on a multi-gigabyte file and nothing large is loaded into memory.

    Usage:
        powershell -ExecutionPolicy Bypass -File tiffinfo.ps1 "C:\path\to\scan.tif"
#>

param([Parameter(Mandatory=$true)][string]$Path)

if (-not (Test-Path $Path)) { Write-Host "File not found: $Path" -F Red; exit 1 }

$fs = [System.IO.File]::OpenRead($Path)
$br = New-Object System.IO.BinaryReader($fs)

Write-Host ""
Write-Host "File   : $Path"
Write-Host "Size   : $($fs.Length) bytes ($([math]::Round($fs.Length/1GB,2)) GB)"

# ---- header ----
$b = $br.ReadBytes(2)
$order = [System.Text.Encoding]::ASCII.GetString($b)
if     ($order -eq 'II') { $little = $true }
elseif ($order -eq 'MM') { $little = $false }
else { Write-Host "Not a TIFF (byte order '$order')" -F Red; $fs.Close(); exit 1 }

function RdU16 { $d = $br.ReadBytes(2); if (-not $little) { [array]::Reverse($d) }; [BitConverter]::ToUInt16($d,0) }
function RdU32 { $d = $br.ReadBytes(4); if (-not $little) { [array]::Reverse($d) }; [BitConverter]::ToUInt32($d,0) }
function RdU64 { $d = $br.ReadBytes(8); if (-not $little) { [array]::Reverse($d) }; [BitConverter]::ToUInt64($d,0) }

$magic = RdU16
Write-Host "Order  : $order  ($(if($little){'little'}else{'big'})-endian)"
Write-Host "Magic  : $magic  $(if($magic -eq 42){'(classic TIFF)'}elseif($magic -eq 43){'(BigTIFF)'}else{'(UNKNOWN)'})"

if ($magic -eq 43) {
    $offSize = RdU16; $null = RdU16
    $ifd = RdU64
    $big = $true
} elseif ($magic -eq 42) {
    $ifd = RdU32
    $big = $false
} else { $fs.Close(); exit 1 }

Write-Host "IFD at : $ifd  (0x$('{0:X}' -f $ifd))"
if ($ifd -eq 0) {
    Write-Host ""
    Write-Host "IFD offset is 0 - the file was never finalised (scan crashed or was" -F Yellow
    Write-Host "aborted). Pixel data is still on disk; only the directory is missing." -F Yellow
    $fs.Close(); exit 0
}
if ($ifd -ge $fs.Length) {
    Write-Host ""
    Write-Host "IFD offset points past the end of the file - metadata is corrupt," -F Yellow
    Write-Host "which is exactly the 32-bit overflow symptom." -F Yellow
    $fs.Close(); exit 0
}

# ---- tags ----
$names = @{
  256='ImageWidth'; 257='ImageLength'; 258='BitsPerSample'; 259='Compression';
  262='Photometric'; 273='StripOffsets'; 274='Orientation'; 277='SamplesPerPixel';
  278='RowsPerStrip'; 279='StripByteCounts'; 282='XResolution'; 283='YResolution';
  284='PlanarConfig'; 296='ResolutionUnit'; 305='Software'; 306='DateTime';
  317='Predictor'; 322='TileWidth'; 323='TileLength'; 324='TileOffsets';
  325='TileByteCounts'; 338='ExtraSamples'; 339='SampleFormat'; 34675='ICCProfile'
}
$types = @{1='BYTE';2='ASCII';3='SHORT';4='LONG';5='RATIONAL';6='SBYTE';7='UNDEF';
           8='SSHORT';9='SLONG';10='SRATIONAL';11='FLOAT';12='DOUBLE';16='LONG8';17='SLONG8';18='IFD8'}
$tsize = @{1=1;2=1;3=2;4=4;5=8;6=1;7=1;8=2;9=4;10=8;11=4;12=8;16=8;17=8;18=8}

$fs.Position = $ifd
$n = if ($big) { RdU64 } else { RdU16 }
Write-Host "Entries: $n"
Write-Host ""
Write-Host ("{0,-6} {1,-18} {2,-8} {3,-8} {4}" -f 'Tag','Name','Type','Count','Value')
Write-Host ("-"*72)

for ($i=0; $i -lt $n; $i++) {
    $tag  = RdU16
    $typ  = RdU16
    $cnt  = if ($big) { RdU64 } else { RdU32 }
    $inlineBytes = if ($big) { 8 } else { 4 }
    $sz = $tsize[[int]$typ]; if (-not $sz) { $sz = 1 }
    $total = $cnt * $sz

    $raw = $br.ReadBytes($inlineBytes)
    $val = ''
    if ($total -le $inlineBytes) {
        $d = $raw[0..([math]::Min($sz*[math]::Min($cnt,4),$inlineBytes)-1)]
        if (-not $little) { [array]::Reverse($d) }
        switch ($sz) {
            2 { $val = [BitConverter]::ToUInt16($d,0) }
            4 { $val = [BitConverter]::ToUInt32($d,0) }
            8 { $val = [BitConverter]::ToUInt64($d,0) }
            default { $val = ($raw | ForEach-Object { $_ }) -join ',' }
        }
        if ($cnt -gt 1 -and $sz -lt 4) { $val = "$val (+$($cnt-1) more)" }
    } else {
        $o = $raw; if (-not $little) { [array]::Reverse($o) }
        $off = if ($big) { [BitConverter]::ToUInt64($o,0) } else { [BitConverter]::ToUInt32($o,0) }
        $val = "-> offset $off ($cnt values)"
    }

    $nm = $names[[int]$tag]; if (-not $nm) { $nm = '' }
    $tn = $types[[int]$typ]; if (-not $tn) { $tn = "?$typ" }
    Write-Host ("{0,-6} {1,-18} {2,-8} {3,-8} {4}" -f $tag,$nm,$tn,$cnt,$val)
}

$next = if ($big) { RdU64 } else { RdU32 }
Write-Host ""
Write-Host "Next IFD: $next"
$fs.Close()
Write-Host ""
