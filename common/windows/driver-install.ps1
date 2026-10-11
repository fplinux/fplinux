# SPDX-License-Identifier: GPL-2.0-only
param(
    [string]$RequestPath
)

$ErrorActionPreference = 'Stop'
$Request = Get-Content -LiteralPath $RequestPath -Raw -Encoding UTF8 | ConvertFrom-Json
Set-Location -LiteralPath $Request.directory

$ErrorActionPreference = 'Continue'
& $Request.installer --type 0 --vid 0x1782 --pid 0x4d00 `
    --name 'FPLinux BootROM' --inf fplinux-bootrom.inf --dest driver 2>&1 |
    Out-File -LiteralPath $Request.log -Encoding UTF8 -Append -Force
exit $LASTEXITCODE
