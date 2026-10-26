# SPDX-License-Identifier: GPL-2.0-only
param(
    [Parameter(Mandatory = $true)][string]$Python,
    [Parameter(Mandatory = $true)][string]$Checkout,
    [Parameter(Mandatory = $true)][string]$Distribution
)
$ErrorActionPreference = 'Stop'
$account = 'fplinux_smoke'
$password = ConvertTo-SecureString ([guid]::NewGuid().ToString() + 'aA1!') -AsPlainText -Force
$credential = [pscredential]::new("$env:COMPUTERNAME\$account", $password)
$workspace = Join-Path $env:RUNNER_TEMP 'fplinux-normal-user'
if (Test-Path $workspace) { throw 'The ordinary-user smoke workspace already exists' }
New-Item -ItemType Directory $workspace | Out-Null
$created = $false
try {
    $user = New-LocalUser -Name $account -Password $password -AccountNeverExpires
    $created = $true
    if (-not (Get-LocalGroupMember -SID 'S-1-5-32-545' | Where-Object { $_.SID -eq $user.SID })) {
        Add-LocalGroupMember -SID 'S-1-5-32-545' -Member $user
    }
    & icacls.exe $workspace /grant "*$($user.SID):(OI)(CI)M" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot grant the ordinary user access to the smoke workspace' }
    & icacls.exe $workspace /grant '*S-1-5-11:(OI)(CI)RX' '*S-1-5-32-545:(OI)(CI)RX' | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare inherited archive permissions' }
    Copy-Item -Recurse (Join-Path $Checkout 'scripts') (Join-Path $workspace 'scripts')
    Copy-Item -Recurse (Join-Path $Checkout 'targets') (Join-Path $workspace 'targets')
    Copy-Item -Recurse (Join-Path $Checkout 'platforms') (Join-Path $workspace 'platforms')
    Copy-Item -Recurse (Join-Path $Checkout 'profiles') (Join-Path $workspace 'profiles')
    Copy-Item -Recurse (Join-Path $Checkout 'common') (Join-Path $workspace 'common')
    Copy-Item (Join-Path $Checkout 'sources.lock.toml') (Join-Path $workspace 'sources.lock.toml')
    Copy-Item -Recurse $Distribution (Join-Path $workspace 'distribution')
    $program = Join-Path $workspace 'smoke.py'
    Copy-Item (Join-Path $Checkout '.github/scripts/smoke-normal-user.py') $program
    Start-Service seclogon
    $process = Start-Process -FilePath $Python -ArgumentList @('-B', '-I', ('"' + $program + '"')) -Credential $credential -LoadUserProfile -WorkingDirectory $workspace -RedirectStandardOutput (Join-Path $workspace 'stdout.log') -RedirectStandardError (Join-Path $workspace 'stderr.log') -PassThru -Wait
    Get-Content (Join-Path $workspace 'stdout.log')
    Get-Content (Join-Path $workspace 'stderr.log')
    if ($process.ExitCode -ne 0) { throw "Ordinary-user archive smoke failed with status $($process.ExitCode)" }
} finally {
    if ($created) {
        Remove-LocalUser -Name $account -ErrorAction SilentlyContinue
    }
}
