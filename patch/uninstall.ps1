#requires -version 2
<# Restores the original HDSTI.dll. Run Uninstall.cmd, not this directly. #>
$ErrorActionPreference = 'Stop'
function Say($t,$c='Gray'){ Write-Host "  $t" -ForegroundColor $c }
function Test-Install($p){ if(-not $p){return $false}; try{ Test-Path (Join-Path $p 'NC7000.exe') }catch{ $false } }

Write-Host ''
Write-Host '  Removing the 64-bit scanner patch' -ForegroundColor White
Write-Host ''

$target=$null
foreach($k in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
                'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*')){
  if($target){break}
  try{$items=Get-ItemProperty $k -ErrorAction SilentlyContinue}catch{continue}
  foreach($i in $items){
    if($i.DisplayName -and $i.DisplayName -match 'Newcolor'){
      foreach($c in @($i.InstallLocation,(Split-Path -Parent ($i.UninstallString -replace '"','')))){
        if(Test-Install $c){$target=$c;break}
      }
    }
    if($target){break}
  }
}
if(-not $target){
  foreach($c in @('C:\newcolor','C:\Newcolor 7000 2.0',
                  "$env:ProgramFiles\Newcolor 7000 2.0",
                  "${env:ProgramFiles(x86)}\Newcolor 7000 2.0")){
    if(Test-Install $c){$target=$c;break}
  }
}
if(-not $target){ $target=(Read-Host '  Path to the Newcolor folder').Trim('"').Trim() }

if(-not (Test-Install $target)){ Say "NC7000.exe not found in '$target'." 'Red'; exit 1 }
$stock=Join-Path $target 'HDSTI_stock.dll'
if(-not (Test-Path $stock)){ Say "HDSTI_stock.dll not found - the patch is not installed here." 'Yellow'; exit 0 }
if(Get-Process -Name 'NC7000' -ErrorAction SilentlyContinue){ Say "Newcolor is running. Close it first." 'Red'; exit 1 }

Remove-Item (Join-Path $target 'HDSTI.dll') -Force -ErrorAction SilentlyContinue
Move-Item $stock (Join-Path $target 'HDSTI.dll') -Force
Remove-Item (Join-Path $target 'hdsti.ini') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $target 'hdsti.log') -Force -ErrorAction SilentlyContinue
Say "Original HDSTI.dll restored." 'Green'
Say "The scanner will no longer work on 64-bit Windows without the patch."
Write-Host ''
