# SPDX-License-Identifier: GPL-2.0-only
param(
    [string]$Path
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$Acl = Get-Acl -LiteralPath $Path
$Rules = @($Acl.Access | ForEach-Object {
    @{
        sid = $_.IdentityReference.Translate([Security.Principal.SecurityIdentifier]).Value
        type = [string]$_.AccessControlType
    }
})
$Result = @{
    owner = $Acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    protected = $Acl.AreAccessRulesProtected
    rules = $Rules
}
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
