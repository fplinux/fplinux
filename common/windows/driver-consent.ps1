# SPDX-License-Identifier: GPL-2.0-only
param(
    [string]$LicensePath
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

Add-Type -AssemblyName System.Windows.Forms
$Form = New-Object Windows.Forms.Form
$Form.Text = 'FPLinux USB driver setup'
$Form.ClientSize = '640,560'
$Form.StartPosition = 'CenterScreen'
$Form.TopMost = $true
$Form.FormBorderStyle = 'FixedDialog'
$Form.MaximizeBox = $false
$Form.MinimizeBox = $false

$Label = New-Object Windows.Forms.Label
$Label.Location = '16,16'
$Label.Size = '608,138'
$Label.Text = (
    'Install the compatible WinUSB driver for BootROM phones ' +
    'with USB ID 1782:4D00? Administrator approval is required. This replaces ' +
    'the driver for matching phones and adds a local driver-signing certificate ' +
    'to the computer''s trusted certificates (Root and Trusted Publishers). ' +
    'Its signing key is deleted after signing. Restore-point creation is ' +
    'temporarily paused during installation and restored when the installer ' +
    'exits normally. Read the Microsoft license below before continuing.'
)
$Terms = New-Object Windows.Forms.RichTextBox
$Terms.Location = '16,158'
$Terms.Size = '608,292'
$Terms.ReadOnly = $true
$Terms.LoadFile($LicensePath)

$Accept = New-Object Windows.Forms.CheckBox
$Accept.Location = '16,466'
$Accept.Size = '608,26'
$Accept.Text = 'I accept the Microsoft license terms shown above.'
$Install = New-Object Windows.Forms.Button
$Install.Location = '390,510'
$Install.Size = '112,32'
$Install.Text = 'Install'
$Install.Enabled = $false
$Install.DialogResult = [Windows.Forms.DialogResult]::OK
$Cancel = New-Object Windows.Forms.Button
$Cancel.Location = '512,510'
$Cancel.Size = '112,32'
$Cancel.Text = 'Cancel'
$Cancel.DialogResult = [Windows.Forms.DialogResult]::Cancel
$Form.CancelButton = $Cancel
$Accept.Add_CheckedChanged({ $Install.Enabled = $Accept.Checked })
$Form.Controls.AddRange(@($Label, $Terms, $Accept, $Install, $Cancel))

try {
    $Choice = $Form.ShowDialog()
    $Result = @{
        accepted = ($Choice -eq [Windows.Forms.DialogResult]::OK -and $Accept.Checked)
    }
} finally {
    $Form.Dispose()
}
ConvertTo-Json -InputObject $Result -Depth 12 -Compress
