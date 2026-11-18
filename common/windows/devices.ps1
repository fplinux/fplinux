# SPDX-License-Identifier: GPL-2.0-only
param(
    [switch]$DriverInfo,
    [UInt16]$VendorId,
    [UInt16]$ProductId
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$RootPattern = '^USB\\VID_[0-9A-F]{4}&PID_[0-9A-F]{4}(?:&REV_[0-9A-F]{4})?\\'
if ($DriverInfo) {
    $RootPattern = '^USB\\VID_{0:X4}&PID_{1:X4}(?:&REV_[0-9A-F]{{4}})?\\[^\\]+$' -f $VendorId, $ProductId
}
$Devices = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -match $RootPattern
} | ForEach-Object {
    $Device = @{
        id = $_.InstanceId
    }
    if ($DriverInfo) {
        $Problem = @(Get-PnpDeviceProperty -InstanceId $_.InstanceId `
            -KeyName DEVPKEY_Device_ProblemCode -ErrorAction SilentlyContinue)
        if ($Problem.Count -eq 1) {
            $Device.problem_code = [int]$Problem[0].Data
        }
    }
    $Device
})
$Result = @{ devices = $Devices }
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
