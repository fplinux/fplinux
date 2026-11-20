# SPDX-License-Identifier: GPL-2.0-only
param()

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$Result = @{
    sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
}
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
