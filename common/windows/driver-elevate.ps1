# SPDX-License-Identifier: GPL-2.0-only
param(
    [string]$RequestPath
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

$Request = Get-Content -LiteralPath $RequestPath -Raw -Encoding UTF8 | ConvertFrom-Json
$PowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$InstallScript = Join-Path $PSScriptRoot 'driver-install.ps1'
# Start-Process joins its argument array into a native Windows command line.
$Arguments = @(
    '-NoLogo'
    '-NoProfile'
    '-NonInteractive'
    '-ExecutionPolicy'
    'Bypass'
    '-File'
    ('"{0}"' -f $InstallScript)
    '-RequestPath'
    ('"{0}"' -f $RequestPath)
)
try {
    $Process = Start-Process -FilePath $PowerShell -ArgumentList $Arguments `
        -WorkingDirectory $Request.directory -Verb RunAs -Wait -PassThru
    $Result = @{ cancelled = $false; status = $Process.ExitCode }
} catch {
    $Failure = $_.Exception
    $Cancelled = $false
    while ($Failure) {
        if ($Failure -is [ComponentModel.Win32Exception] -and $Failure.NativeErrorCode -eq 1223) {
            $Cancelled = $true
        }
        $Failure = $Failure.InnerException
    }
    if ($Cancelled) {
        $Result = @{ cancelled = $true }
    } else {
        throw
    }
}
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
