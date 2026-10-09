# SPDX-License-Identifier: GPL-2.0-only
param()

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$Devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match '^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}(?:&REV_[0-9A-F]{4})?\\'
} | ForEach-Object {
    $Service = (Get-PnpDeviceProperty -InstanceId $_.InstanceId `
        -KeyName DEVPKEY_Device_Service -ErrorAction SilentlyContinue).Data
    @{
        id = $_.InstanceId
        status = $_.Status
        service = [string]$Service
    }
})
$Result = @{ devices = $Devices }
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
