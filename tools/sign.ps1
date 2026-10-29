param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Files)
$ErrorActionPreference = 'Stop'

# Machine-specific, kept out of git: sign.local.json = { "Dlib": "<Azure.CodeSigning.Dlib.dll>", "Metadata": "<metadata.json>" }
$cfg = Get-Content (Join-Path $PSScriptRoot 'sign.local.json') -Raw | ConvertFrom-Json
$kit = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\signtool.exe' | Sort-Object FullName | Select-Object -Last 1

# The dlib authenticates from these variables; without them it opens an interactive login.
$m = Get-Content $cfg.Metadata -Raw | ConvertFrom-Json
foreach ($n in 'AZURE_CLIENT_ID', 'AZURE_TENANT_ID', 'AZURE_CLIENT_SECRET') { if ($m.$n) { Set-Item "env:$n" $m.$n } }

& $kit.FullName sign /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib $cfg.Dlib /dmdf $cfg.Metadata $Files
if ($LASTEXITCODE -ne 0) { throw 'signing failed' }
& $kit.FullName verify /pa $Files
if ($LASTEXITCODE -ne 0) { throw 'verify failed' }
