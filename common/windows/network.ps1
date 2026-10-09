# SPDX-License-Identifier: GPL-2.0-only
param(
    [string]$HostMac
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$MatchingAdapters = @(Get-NetAdapter -IncludeHidden | Where-Object {
    ([string]$_.MacAddress -replace '-', ':') -eq $HostMac
})
$PresentDevices = @()
if ($MatchingAdapters.Count -gt 0) {
    $PresentDevices = @(Get-PnpDevice -PresentOnly -Class Net)
}
$Adapters = @($MatchingAdapters | ForEach-Object {
    $Adapter = $_
    $Device = @($PresentDevices | Where-Object {
        $_.InstanceId -eq $Adapter.PnPDeviceID
    })
    if ($Device.Count -eq 1) {
        $Lineage = @()
        $InstanceId = $Device[0].InstanceId
        for ($Depth = 0; $Depth -lt 16 -and $InstanceId; $Depth++) {
            if ($Lineage -contains $InstanceId) {
                break
            }
            $Lineage += $InstanceId
            $Parent = Get-PnpDeviceProperty -InstanceId $InstanceId `
                -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue
            $InstanceId = $Parent.Data
        }
        $CompatibleIds = (Get-PnpDeviceProperty -InstanceId $Device[0].InstanceId `
            -KeyName DEVPKEY_Device_CompatibleIds -ErrorAction SilentlyContinue).Data
        $Provider = (Get-PnpDeviceProperty -InstanceId $Device[0].InstanceId `
            -KeyName DEVPKEY_Device_DriverProvider -ErrorAction SilentlyContinue).Data
        $IpInterface = Get-NetIPInterface -InterfaceIndex $Adapter.ifIndex `
            -AddressFamily IPv4 -ErrorAction SilentlyContinue
        $Addresses = @(Get-NetIPAddress -InterfaceIndex $Adapter.ifIndex `
            -AddressFamily IPv4 -PolicyStore ActiveStore -ErrorAction SilentlyContinue |
            ForEach-Object {
                @{
                    address = $_.IPAddress
                    prefix = [int]$_.PrefixLength
                    state = [string]$_.AddressState
                    origin = [string]$_.PrefixOrigin
                }
            })
        @{
            index = [int]$Adapter.ifIndex
            mac = [string]$Adapter.MacAddress
            status = [string]$Adapter.Status
            compatible = @($CompatibleIds)
            provider = [string]$Provider
            lineage = @($Lineage)
            dhcp = [string]$IpInterface.Dhcp
            addresses = $Addresses
        }
    }
})
$Routes = @(Get-NetRoute -AddressFamily IPv4 -PolicyStore ActiveStore |
    ForEach-Object {
        @{
            prefix = $_.DestinationPrefix
            next_hop = $_.NextHop
            index = [int]$_.InterfaceIndex
        }
    })
$Result = @{ adapters = $Adapters; routes = $Routes }
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
