# SPDX-License-Identifier: GPL-2.0-only
param()

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$Routes = @(Get-NetRoute -AddressFamily IPv4 -PolicyStore ActiveStore |
    ForEach-Object {
        @{
            prefix = $_.DestinationPrefix
            next_hop = $_.NextHop
            index = [int]$_.InterfaceIndex
        }
    })
$Result = @{ routes = $Routes }
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
