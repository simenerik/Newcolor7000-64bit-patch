<#
    Newcolor 7000 - 64-bit Windows scanner patch
    Installs the replacement HDSTI.dll.

    Finds the installation by looking, in order, at:
      1. the folder this script is in
      2. the Windows uninstall registry
      3. a list of common locations
      4. a bounded search of the Program Files and drive roots
      5. asking

    Run Install.cmd rather than calling this directly - it handles
    elevation and execution policy.
#>

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Say($t, $c = 'Gray') { Write-Host "  $t" -ForegroundColor $c }

function Test-Install($p) {
    if (-not $p) { return $false }
    try { return (Test-Path (Join-Path $p 'NC7000.exe')) } catch { return $false }
}

Write-Host ''
Write-Host '  Newcolor 7000 - 64-bit scanner patch' -ForegroundColor White
Write-Host '  ====================================' -ForegroundColor White
Write-Host ''

# ---- 1. this folder ----------------------------------------------------
$target = $null
if (Test-Install $here) { $target = $here; Say "Found alongside this script." }

# ---- 2. uninstall registry ---------------------------------------------
if (-not $target) {
    $keys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($k in $keys) {
        if ($target) { break }
        try { $items = Get-ItemProperty $k -ErrorAction SilentlyContinue } catch { continue }
        foreach ($i in $items) {
            if ($i.DisplayName -and $i.DisplayName -match 'Newcolor') {
                foreach ($cand in @($i.InstallLocation,
                                    (Split-Path -Parent ($i.UninstallString -replace '"','')))) {
                    if (Test-Install $cand) {
                        $target = $cand
                        Say "Found via the uninstall registry."
                        break
                    }
                }
            }
            if ($target) { break }
        }
    }
}

# ---- 3. common locations -----------------------------------------------
if (-not $target) {
    $common = @('C:\newcolor', 'C:\Newcolor', 'C:\Newcolor 7000 2.0', 'C:\NC7000',
                "$env:ProgramFiles\Newcolor 7000 2.0",
                "${env:ProgramFiles(x86)}\Newcolor 7000 2.0",
                "$env:ProgramFiles\Heidelberg\Newcolor 7000 2.0",
                "${env:ProgramFiles(x86)}\Heidelberg\Newcolor 7000 2.0")
    foreach ($c in $common) {
        if (Test-Install $c) { $target = $c; Say "Found in a standard location."; break }
    }
}

# ---- 4. bounded search --------------------------------------------------
if (-not $target) {
  if ($PSVersionTable.PSVersion.Major -lt 5) {
    Say "Skipping the deep search - it needs PowerShell 5 or newer." 'Yellow'
  } else {
    Say "Searching for NC7000.exe, this takes a moment..." 'Yellow'
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
            $hit = Get-ChildItem -Path $r -Filter 'NC7000.exe' -Recurse -Depth 3 `
                                 -File -Force -ErrorAction SilentlyContinue |
                   Select-Object -First 1
        } catch { $hit = $null }
        if ($hit) { $target = $hit.DirectoryName; Say "Found by search." }
    }
  }
}

# ---- 5. ask -------------------------------------------------------------
if (-not $target) {
    Write-Host ''
    Say "Could not find Newcolor automatically." 'Yellow'
    $target = Read-Host '  Full path to the folder containing NC7000.exe'
    $target = $target.Trim('"').Trim()
}

if (-not (Test-Install $target)) {
    Write-Host ''
    Say "NC7000.exe not found in '$target'. Nothing has been changed." 'Red'
    exit 1
}

Say "Installation: $target" 'White'

# ---- sanity checks ------------------------------------------------------
if (-not (Test-Path (Join-Path $target 'KSS32.dll'))) {
    Write-Host ''
    Say "KSS32.dll is missing. This looks like a demo-only install with no" 'Red'
    Say "scanner modules, so the patch would have no effect." 'Red'
    exit 1
}

if (Get-Process -Name 'NC7000' -ErrorAction SilentlyContinue) {
    Write-Host ''
    Say "Newcolor is running. Close it and run this again." 'Red'
    exit 1
}

if ($target -match [regex]::Escape('Program Files')) {
    Write-Host ''
    Say "NOTE: Newcolor is installed under Program Files." 'Yellow'
    Say "It writes its licence file into its own folder, which Windows may" 'Yellow'
    Say "redirect or block there. If you hit licence errors that look like a" 'Yellow'
    Say "bad serial, reinstall Newcolor somewhere like C:\newcolor instead." 'Yellow'
    Say "The patch itself works either way." 'Yellow'
}

# ---- back up, once ------------------------------------------------------
$stock = Join-Path $target 'HDSTI_stock.dll'
$live  = Join-Path $target 'HDSTI.dll'

Write-Host ''
if (Test-Path $stock) {
    Say "Original already backed up as HDSTI_stock.dll - keeping it."
} elseif (Test-Path $live) {
    Move-Item $live $stock -Force
    Say "Backed up the original to HDSTI_stock.dll"
}

# ---- install ------------------------------------------------------------
Copy-Item (Join-Path $here 'HDSTI.dll') $live -Force
Say "Installed HDSTI.dll" 'Green'

$ini = Join-Path $target 'hdsti.ini'
if (Test-Path $ini) {
    Say "hdsti.ini already present - keeping your settings."
    Say "Delete it and re-run to pick up new defaults."
} else {
    Copy-Item (Join-Path $here 'hdsti.ini') $ini -Force
    Say "Installed hdsti.ini" 'Green'
}

try { Unblock-File $live -ErrorAction SilentlyContinue } catch { }

Write-Host ''
Write-Host '  Done.' -ForegroundColor Green
Write-Host ''
Say "Next:"
Say "  1. Switch the scanner ON BEFORE booting the PC."
Say "     SCSI is enumerated at startup."
Say "  2. Run Newcolor AS ADMINISTRATOR. Pass-through needs it."
Say "  3. Pick your scanner under Input source."
Write-Host ''
Say "Do NOT install the HDHLusd driver. It is 32-bit, Windows x64 will"
Say "refuse it, and this patch removes the need for it."
Write-Host ''
Say "Trouble? Set Log=1 in hdsti.ini, reproduce, read hdsti.log."
Write-Host ''
