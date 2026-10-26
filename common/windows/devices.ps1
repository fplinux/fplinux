# SPDX-License-Identifier: GPL-2.0-only
param()

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$Devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match '^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}(?:&REV_[0-9A-F]{4})?\\'
} | ForEach-Object {
    @{
        id = $_.InstanceId
    }
})
$Result = @{ devices = $Devices }
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
