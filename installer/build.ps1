$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$stage = Join-Path $PSScriptRoot 'stage'
$sign = Join-Path $root 'tools\sign.ps1'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null

# Built straight into stage: the shim folder's copy is locked while a J2534 app has it loaded.
cmd /c (Join-Path $root 'shim\build.bat') (Join-Path $stage 'op20winusb.dll')
if ($LASTEXITCODE -ne 0) { throw 'shim build failed' }
& $sign (Join-Path $stage 'op20winusb.dll')

& (Join-Path $root 'driver\build.ps1')
Copy-Item (Join-Path $root 'driver\openport_winusb.inf'), (Join-Path $root 'driver\openport_winusb.cat') $stage
$signer = (Get-AuthenticodeSignature (Join-Path $stage 'openport_winusb.cat')).SignerCertificate
[IO.File]::WriteAllBytes((Join-Path $stage 'publisher.cer'), $signer.Export('Cert'))

# Tactrix's last release, unmodified.
$tactrix = 'C:\Program Files (x86)\OpenECU\OpenPort 2.0\drivers\openport 2.0'
Copy-Item (Join-Path $tactrix 'op20pt32.dll') (Join-Path $stage 'op20pt32_tactrix.dll')
Copy-Item (Join-Path $tactrix 'License.txt') (Join-Path $stage 'Tactrix-License.txt')
Copy-Item (Join-Path $root 'LICENSE') $stage

Push-Location $PSScriptRoot
& 'C:\Program Files (x86)\NSIS\makensis.exe' /V2 openport_winusb.nsi
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw 'makensis failed' }
$exe = Get-ChildItem (Join-Path $stage 'OpenPort20WinUSB-*.exe')
& $sign $exe.FullName
"Built $($exe.FullName)"
