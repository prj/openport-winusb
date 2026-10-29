param([switch]$NoSign)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$kit = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\makecat.exe' | Sort-Object FullName | Select-Object -Last 1
Remove-Item openport_winusb.cat -ErrorAction SilentlyContinue
& $kit.FullName -v openport_winusb.cdf
if ($LASTEXITCODE -ne 0 -or -not (Test-Path openport_winusb.cat)) { throw 'makecat failed' }
if ($NoSign) { return }

& (Join-Path $PSScriptRoot '..\tools\sign.ps1') openport_winusb.cat

# Proves the catalog covers the INF as it is now.
& (Join-Path $kit.DirectoryName 'signtool.exe') verify /pa /c openport_winusb.cat openport_winusb.inf
if ($LASTEXITCODE -ne 0) { throw 'catalog does not cover the INF' }
