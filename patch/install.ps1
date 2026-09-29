<#
    install.ps1 - Newcolor 7000 64-bit scanner patch

    Run Install.cmd rather than this directly; it handles elevation and
    execution policy.

    Finds Newcolor wherever it happens to be, installs the patch, and checks
    afterwards that what is on disk is what was meant to be there.
#>

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Say  ($t, $c = 'Gray') { Write-Host "  $t" -ForegroundColor $c }
function Head ($t) {
    Write-Host ''
    Write-Host "  $t" -ForegroundColor White
    Write-Host ("  " + ("-" * $t.Length)) -ForegroundColor DarkGray
}

function Test-Install ($p) {
    if (-not $p) { return $false }
    try { return (Test-Path (Join-Path $p 'NC7000.exe')) } catch { return $false }
}

Write-Host ''
Write-Host '  ===================================================' -ForegroundColor White
Write-Host '   Newcolor 7000 - 64-bit scanner patch' -ForegroundColor White
Write-Host '  ===================================================' -ForegroundColor White

# ---------------------------------------------------------------- find it ---
Head 'Looking for Newcolor'

$target = $null
$how    = ''

# 1. dropped next to NC7000.exe?
if (Test-Install $here) { $target = $here; $how = 'alongside this installer' }

# 2. the uninstall registry - works wherever it was installed
if (-not $target) {
    foreach ($k in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                     'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
        if ($target) { break }
        try { $items = Get-ItemProperty $k -ErrorAction SilentlyContinue } catch { continue }
        foreach ($i in $items) {
            if ($i.DisplayName -and $i.DisplayName -match 'Newcolor') {
                foreach ($c in @($i.InstallLocation,
                                 (Split-Path -Parent ($i.UninstallString -replace '"','')))) {
                    if (Test-Install $c) { $target = $c; $how = 'from the Windows uninstall list'; break }
                }
            }
            if ($target) { break }
        }
    }
}

# 3. places people actually put it
if (-not $target) {
    $common = @('C:\newcolor', 'C:\Newcolor', 'C:\NewColor', 'C:\Newcolor 7000 2.0',
                'C:\NC7000', 'D:\newcolor', 'D:\Newcolor 7000 2.0',
                "$env:ProgramFiles\Newcolor 7000 2.0",
                "${env:ProgramFiles(x86)}\Newcolor 7000 2.0",
                "$env:ProgramFiles\Heidelberg\Newcolor 7000 2.0",
                "${env:ProgramFiles(x86)}\Heidelberg\Newcolor 7000 2.0",
                "${env:ProgramFiles(x86)}\Heidelberg Digital\Newcolor 7000 2.0")
    foreach ($c in $common) {
        if (Test-Install $c) { $target = $c; $how = 'in a standard location'; break }
    }
}

# 4. look for it
if (-not $target -and $PSVersionTable.PSVersion.Major -ge 5) {
    Say 'Not in the usual places - searching. This takes a moment...' 'Yellow'
    $roots = @()
    foreach ($r in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($r -and (Test-Path $r)) { $roots += $r }
    }
    foreach ($d in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        if ($d.Root -and (Test-Path $d.Root)) { $roots += $d.Root }
    }
    foreach ($r in ($roots | Select-Object -Unique)) {
        if ($target) { break }
        try {
            $hit = Get-ChildItem -Path $r -Filter 'NC7000.exe' -Recurse -Depth 4 -File -Force -ErrorAction SilentlyContinue |
                   Select-Object -First 1
        } catch { $hit = $null }
        if ($hit) { $target = $hit.DirectoryName; $how = 'by searching the disk' }
    }
}

# 5. ask
if (-not $target) {
    Write-Host ''
    Say 'Could not find Newcolor automatically.' 'Yellow'
    Say 'Open the folder that contains NC7000.exe, copy the address bar, paste it here.'
    Write-Host ''
    $target = (Read-Host '  Folder').Trim().Trim('"')
}

if (-not (Test-Install $target)) {
    Write-Host ''
    Say "NC7000.exe is not in '$target'." 'Red'
    Say 'Nothing has been changed.' 'Red'
    Write-Host ''
    Say 'Install Newcolor 7000 first, then run this again.'
    Say 'See README.md - the disc has an installer that will not'
    Say 'start on 64-bit Windows, and a working one inside the Setup folder.'
    Write-Host ''
    exit 1
}

Say "Found $how" 'Green'
Say $target 'White'

# ----------------------------------------------------------------- checks ---
Head 'Checking'

$ok = $true

if (Test-Path (Join-Path $target 'KSS32.dll')) {
    Say 'Scanner modules present' 'Green'
} else {
    Say 'KSS32.dll is missing.' 'Red'
    Say 'This looks like a demo-only install with no scanner support, so the'
    Say 'patch would have nothing to attach to. Reinstall Newcolor and enable'
    Say 'the scanners you own.'
    $ok = $false
}

if (Get-Process -Name 'NC7000' -ErrorAction SilentlyContinue) {
    Say 'Newcolor is running - close it and run this again.' 'Red'
    $ok = $false
}

if (-not $ok) { Write-Host ''; exit 1 }

if ($target -match [regex]::Escape('Program Files')) {
    Write-Host ''
    Say 'NOTE: Newcolor is installed under Program Files.' 'Yellow'
    Say 'It writes its licence file into its own folder, which Windows blocks' 'Yellow'
    Say 'there. If you later get licence errors that look like a bad serial'  'Yellow'
    Say 'number, that is the cause - reinstall to somewhere like C:\newcolor.' 'Yellow'
    Say 'The patch itself works either way.'                                   'Yellow'
}

# ---------------------------------------------------------------- install ---
Head 'Installing'

$live  = Join-Path $target 'HDSTI.dll'
$stock = Join-Path $target 'HDSTI_stock.dll'
$ini   = Join-Path $target 'hdsti.ini'

if (Test-Path $stock) {
    Say 'Original already saved as HDSTI_stock.dll - leaving it alone'
} elseif (Test-Path $live) {
    Move-Item $live $stock -Force
    Say 'Saved the original as HDSTI_stock.dll' 'Green'
} else {
    Say 'No HDSTI.dll found to back up - continuing' 'Yellow'
}

Copy-Item (Join-Path $here 'HDSTI.dll') $live -Force
Say 'Installed HDSTI.dll' 'Green'

if (Test-Path $ini) {
    Say 'hdsti.ini already there - keeping your settings'
    Say '(delete it and run this again to get the defaults back)'
} else {
    Copy-Item (Join-Path $here 'hdsti.ini') $ini -Force
    Say 'Installed hdsti.ini' 'Green'
}

try { Unblock-File $live -ErrorAction SilentlyContinue } catch { }

# ----------------------------------------------------------------- verify ---
Head 'Verifying'

$srcLen = (Get-Item (Join-Path $here 'HDSTI.dll')).Length
$dstLen = (Get-Item $live).Length
if ($srcLen -eq $dstLen) {
    Say "HDSTI.dll is $dstLen bytes, matching the package" 'Green'
} else {
    Say "HDSTI.dll is $dstLen bytes but the package has $srcLen." 'Red'
    Say 'The copy did not land. Is Newcolor still running?' 'Red'
    Write-Host ''
    exit 1
}
if (Test-Path $ini)   { Say 'hdsti.ini present' 'Green' }
if (Test-Path $stock) { Say 'HDSTI_stock.dll kept, so Uninstall.cmd can undo this' 'Green' }

# ------------------------------------------------------------------- next ---
Head 'Done - three things to remember'

Write-Host ''
Say '1. Switch the scanner ON, then start the PC.' 'White'
Say '   Windows only looks for SCSI devices while booting. A scanner turned'
Say '   on afterwards will not be found.'
Write-Host ''
Say '2. Run Newcolor as administrator.' 'White'
Say '   Right-click the shortcut, Run as administrator. Without this the'
Say '   scanner will not be found no matter what else is correct.'
Write-Host ''
Say '3. Do not install the HDHLusd driver.' 'White'
Say '   It is 32-bit, Windows refuses it, and this patch replaces it.'
Write-Host ''
Say 'Scanning larger than 4 GB? Set BigTIFFOutput=1 in:'
Say "  $ini" 'White'
Write-Host ''
Say 'Scanner not found? Run tools\scsiscan32.exe from an admin prompt.'
Say 'Uninstall.cmd puts the original back.'
Write-Host ''
