param([Parameter(Mandatory)][ValidateSet('install', 'uninstall')][string]$Action)
$ErrorActionPreference = 'Stop'

# The installer is 32-bit; Sysnative reaches the 64-bit pnputil from it.
$pnputil = "$env:windir\Sysnative\pnputil.exe"
if (-not (Test-Path $pnputil)) { $pnputil = "$env:windir\System32\pnputil.exe" }

# Driver store packages for the OpenPort: ours by its catalog name, the rest are Tactrix's and Zadig's.
function Packages([bool]$Ours) {
    Get-ChildItem "$env:windir\INF\oem*.inf" | Where-Object {
        $t = Get-Content $_.FullName -Raw
        $t -match 'VID_0403&PID_CC4[CD]' -and ($t -match 'openport_winusb\.cat') -eq $Ours
    } | ForEach-Object Name
}

function PnpUtil {
    & $pnputil @args | Write-Output
    # 3010: reboot needed; 259: no device present to update.
    if ($LASTEXITCODE -notin 0, 259, 3010) { throw "pnputil $args failed: $LASTEXITCODE" }
}

if ($Action -eq 'install') {
    # Trusting the catalog's signer lets PnP install without the "install this device software?" prompt.
    $cer = Join-Path $PSScriptRoot 'publisher.cer'
    if (Test-Path $cer) { & certutil.exe -f -addstore TrustedPublisher $cer | Out-Null }
    foreach ($p in Packages $false) { PnpUtil /delete-driver $p /uninstall /force }
    PnpUtil /add-driver (Join-Path $PSScriptRoot 'openport_winusb.inf') /install
} else {
    foreach ($p in Packages $true) { PnpUtil /delete-driver $p /uninstall /force }
}
