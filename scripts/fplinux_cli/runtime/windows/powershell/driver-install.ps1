# SPDX-License-Identifier: GPL-2.0-only
param(
    [string]$RequestPath
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$Request = Get-Content -LiteralPath $RequestPath -Raw -Encoding UTF8 | ConvertFrom-Json
Set-Location -LiteralPath $Request.directory

$Principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
@(
    ('Driver setup administrator role: {0}' -f $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    ('Driver setup location: {0}' -f (Get-Location).ProviderPath)
    ('Driver setup process directory: {0}' -f [Environment]::CurrentDirectory)
    ('Driver extraction target: {0}' -f $Request.driver_directory)
) | Out-File -LiteralPath $Request.log -Encoding UTF8 -Append -Force

$ErrorActionPreference = 'Continue'
& $Request.installer --log 0 --type 0 --vid 0x1782 --pid 0x4d00 `
    --name 'FPLinux BootROM' --inf fplinux-bootrom.inf --dest $Request.driver_directory 2>&1 |
    Out-File -LiteralPath $Request.log -Encoding UTF8 -Append -Force
exit $LASTEXITCODE
