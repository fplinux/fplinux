# SPDX-License-Identifier: GPL-2.0-only
param(
    [Parameter(Mandatory = $true)]
    [string]$HostMac,
    [Parameter(Mandatory = $true)]
    [string]$UsbSerial,
    [Parameter(Mandatory = $true)]
    [UInt16]$VendorId,
    [Parameter(Mandatory = $true)]
    [UInt16]$ProductId
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$MatchingAdapters = @(Get-NetAdapter -IncludeHidden | Where-Object {
        ([string]$_.MacAddress -replace '-', ':') -eq $HostMac
    })
$RootPattern = '^USB\\VID_{0:X4}&PID_{1:X4}(?:&REV_[0-9A-F]{{4}})?\\{2}$' -f `
    $VendorId, $ProductId, [regex]::Escape($UsbSerial)
$Adapters = @($MatchingAdapters | ForEach-Object {
        $Adapter = $_
        $Device = @(Get-PnpDevice -PresentOnly -Class Net -InstanceId $Adapter.PnPDeviceID `
                -ErrorAction SilentlyContinue | Where-Object {
                $_.InstanceId -eq $Adapter.PnPDeviceID
            })
        if ($Device.Count -eq 1) {
            $Properties = @{}
            Get-PnpDeviceProperty -InstanceId $Device[0].InstanceId -KeyName @(
                'DEVPKEY_Device_Parent',
                'DEVPKEY_Device_CompatibleIds',
                'DEVPKEY_Device_DriverProvider'
            ) -ErrorAction SilentlyContinue | ForEach-Object {
                $Properties[$_.KeyName] = $_.Data
            }
            $Lineage = @()
            $InstanceId = $Device[0].InstanceId
            for ($Depth = 0; $Depth -lt 16 -and $InstanceId; $Depth++) {
                if ($Lineage -contains $InstanceId) {
                    break
                }
                $Lineage += $InstanceId
                if ($InstanceId -match $RootPattern) {
                    break
                }
                if ($Depth -eq 0) {
                    $InstanceId = $Properties['DEVPKEY_Device_Parent']
                } else {
                    $Parent = Get-PnpDeviceProperty -InstanceId $InstanceId `
                        -KeyName DEVPKEY_Device_Parent -ErrorAction SilentlyContinue
                    $InstanceId = $Parent.Data
                }
            }
            $CompatibleIds = $Properties['DEVPKEY_Device_CompatibleIds']
            $Provider = $Properties['DEVPKEY_Device_DriverProvider']
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
